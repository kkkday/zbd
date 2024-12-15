// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN) && defined(LIBZBD)

#include "zbd_zenfs.h"
#include "fs_zenfs.h"
#include "exp.h"

#include <stdlib.h>
#include <fstream>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libzbd/zbd.h>
#include <linux/blkzoned.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>
#include <set>

#include "io_zenfs.h"
#include "rocksdb/env.h"
#include "db/version_set.h"
#include "db/dbformat.h"

#define KB (1024)
#define MB (1024 * KB)

using std::cout;
using std::endl;
using std::fixed;

/* Number of reserved zones for metadata
 * Two non-offline meta zones are needed to be able
 * to roll the metadata log safely. One extra
 * is allocated to cover for one zone going offline.
 */
#define ZENFS_META_ZONES (3)

/* Minimum of number of zones that makes sense */
#define ZENFS_MIN_ZONES (32)

/* Reserved Zone for Zone Cleaning, Set as 5 since there are five types of lietime in Rocksdb*/
#define RESERVED_ZONE_FOR_CLEANING (10)

namespace ROCKSDB_NAMESPACE {

Zone::Zone(ZonedBlockDevice *zbd, struct zbd_zone *z, const uint32_t id)
    : zbd_(zbd),
      zone_id_(id),
      start_(zbd_zone_start(z)),
      max_capacity_(zbd_zone_capacity(z)),
      wp_(zbd_zone_wp(z)),
      open_for_write_(false),
      is_append(false){
  lifetime_ = Env::WLTH_NOT_SET;
  secondary_lifetime_ = Env::WLTH_NOT_SET;
  used_capacity_ = 0;
  capacity_ = 0;
  if (!(zbd_zone_full(z) || zbd_zone_offline(z) || zbd_zone_rdonly(z)))
    capacity_ = zbd_zone_capacity(z) - (zbd_zone_wp(z) - zbd_zone_start(z));
}

bool Zone::IsUsed() { return (used_capacity_ > 0) || open_for_write_; }
uint64_t Zone::GetCapacityLeft() { return capacity_; }
bool Zone::IsFull() { return (capacity_ == 0); }
bool Zone::IsEmpty() { return (wp_ == start_); }
uint64_t Zone::GetZoneNr() { return start_ / zbd_->GetZoneSize(); }

uint64_t ZonedBlockDevice::GetTotalWritten() {
  uint64_t total = 0;
  for (const auto z : io_zones) {
    z->zone_df_lock_.lock();
    total += (z->wp_ - z->start_);
    z->zone_df_lock_.unlock();
  }
  return total;
}

uint64_t ZonedBlockDevice::GetReclaimableSpace() {
  uint64_t reclaimable = 0;
  for (const auto z : io_zones) {
    if (z->IsFull()) reclaimable += (z->max_capacity_ - z->used_capacity_);
  }
  return reclaimable;
}

uint64_t ZonedBlockDevice::GetUsedSpace() {
  uint64_t used = 0;
  for (const auto z : io_zones) {
    used += z->used_capacity_;
  }
  return used;
}

uint64_t ZonedBlockDevice::GetFreeSpace() {
  uint64_t free = 0;
  for (const auto z : io_zones) {
    free += z->capacity_;
  }
  return free;
}


void Zone::CloseWR() {

  assert(open_for_write_);
  open_for_write_ = false;
  if (Close().ok()) {
    zbd_->NotifyIOZoneClosed();
  }
  if (capacity_ == 0) zbd_->NotifyIOZoneFull();
}

IOStatus Zone::Reset() {
  size_t zone_sz = zbd_->GetZoneSize();
  int fd = zbd_->GetWriteFD();
  unsigned int report = 1;
  struct zbd_zone z;
  int ret;

  assert(!IsUsed());

  ret = zbd_reset_zones(fd, start_, zone_sz);
  if (ret) return IOStatus::IOError("Zone reset failed\n");

  ret = zbd_report_zones(fd, start_, zone_sz, ZBD_RO_ALL, &z, &report);

  if (ret || (report != 1)) return IOStatus::IOError("Zone report failed\n");

  if (zbd_zone_offline(&z))
    capacity_ = 0;
  else
    max_capacity_ = capacity_ = zbd_zone_capacity(&z);

  wp_ = start_;
  lifetime_ = Env::WLTH_NOT_SET;

  for(auto ext : extent_info_){
    delete ext;
  }
  extent_info_.clear();
  return IOStatus::OK();
}

IOStatus Zone::Finish() {
  size_t zone_sz = zbd_->GetZoneSize();
  int fd = zbd_->GetWriteFD();
  int ret;

  assert(!open_for_write_);

  ret = zbd_finish_zones(fd, start_, zone_sz);
  if (ret) return IOStatus::IOError("Zone finish failed\n");

  capacity_ = 0;
  wp_ = start_ + zone_sz;

  return IOStatus::OK();
}

IOStatus Zone::Close() {
  size_t zone_sz = zbd_->GetZoneSize();
  int fd = zbd_->GetWriteFD();
  int ret;

  assert(!open_for_write_);

  if (!(IsEmpty() || IsFull())) {
    ret = zbd_close_zones(fd, start_, zone_sz);
    if (ret) return IOStatus::IOError("Zone close failed\n");
  }

  return IOStatus::OK();
}

IOStatus Zone::Append(char *data, uint32_t size) {
  char *ptr = data;
  uint32_t left = size;
  int fd = zbd_->GetWriteFD();
  int ret = -1;

  if (capacity_ < size)
    return IOStatus::NoSpace("Not enough capacity for append");

  assert((size % zbd_->GetBlockSize()) == 0);

  while (left) {
    ret = pwrite(fd, ptr, size, wp_);
    if (ret < 0){
        return IOStatus::IOError("Write failed in Zone Append");
    }
    ptr += ret;
    zone_df_lock_.lock();
    wp_ += ret;
    zone_df_lock_.unlock();
    capacity_ -= ret;
    left -= ret;
  }
  return IOStatus::OK();
}

void Zone::Invalidate(ZoneExtent* extent) {

  bool found = false;
  if (extent == nullptr) {
    fprintf(stderr, "Try to invalidate extent which is nullptr!\n");
  }
  for(const auto ex : extent_info_) {
      if(ex->valid_){
          if (ex->extent_ == extent) {
              if (found) {
                  fprintf(stderr, "Duplicate Extent in Invalidate (%p == %p)\n", ex->extent_, extent);
              }
              ex->invalidate();
              found = true;
          }
      }
  }
  if (!found) {
    fprintf(stderr, "Failed to Find extent in the zone\n");
  }
}

void Zone::UpdateSecondaryLifeTime(Env::WriteLifeTimeHint lt, uint64_t length) {

  uint64_t total_length = 0;
  double slt = 0;
  for (const auto e : extent_info_) {
    total_length += e->length_;
  }
  for(const auto e : extent_info_) {
    double weigth = (((double)e->length_)/(total_length));
    slt += (weigth * (double)(e->lt_));
  }
  double w = (((double)length)/(total_length));
  slt += (w * (double)(lt));
  secondary_lifetime_ = slt;
}

ZoneExtent::ZoneExtent(uint64_t start, uint32_t length, Zone *zone)
    : start_(start), length_(length), zone_(zone) {}

Zone *ZonedBlockDevice::GetIOZone(uint64_t offset) {
  for (const auto z : io_zones)
    if (z->start_ <= offset && offset < (z->start_ + zone_sz_)) return z;
  return nullptr;
}

ZonedBlockDevice::ZonedBlockDevice(std::string bdevname,
                                   std::shared_ptr<Logger> logger)
    : filename_("/dev/" + bdevname), logger_(logger), db_ptr_(nullptr) {
  Info(logger_, "New Zoned Block Device: %s", filename_.c_str());
  zc_in_progress_.store(false);
  WR_DATA.store(0);
  LAST_WR_DATA.store(100);
  num_zc_cnt = 0;
  num_reset_cnt = 0;
};

void ZonedBlockDevice::SetDBPointer(DBImpl* db) {
    db_ptr_ = db;
}

IOStatus ZonedBlockDevice::Open(bool readonly) {
  struct zbd_zone *zone_rep;
  unsigned int reported_zones;
  size_t addr_space_sz;
  zbd_info info;
  Status s;
  uint64_t i = 0;
  uint64_t m = 0;
  uint64_t r = 0;
  int ret;
  uint32_t zone_cnt = 0;
  read_f_ = zbd_open(filename_.c_str(), O_RDONLY, &info);
  if (read_f_ < 0) {
    return IOStatus::InvalidArgument("Failed to open zoned block device");
  }

  read_direct_f_ = zbd_open(filename_.c_str(), O_RDONLY, &info);
  if (read_f_ < 0) {
    return IOStatus::InvalidArgument("Failed to open zoned block device");
  }

  if (readonly) {
    write_f_ = -1;
  } else {
    write_f_ = zbd_open(filename_.c_str(), O_WRONLY | O_DIRECT, &info);
    if (write_f_ < 0) {
      return IOStatus::InvalidArgument("Failed to open zoned block device");
    }
  }

  if (info.model != ZBD_DM_HOST_MANAGED) {
    return IOStatus::NotSupported("Not a host managed block device");
  }

  if (info.nr_zones < ZENFS_MIN_ZONES) {
    return IOStatus::NotSupported(
        "To few zones on zoned block device (32 required)");
  }

  block_sz_ = info.pblock_size;
  zone_sz_ = info.zone_size;
  nr_zones_ = info.nr_zones;

  /* We need one open zone for meta data writes, the rest can be used for files
   */
  if (info.max_nr_active_zones == 0)
    max_nr_active_io_zones_ = info.nr_zones;
  else
    max_nr_active_io_zones_ = info.max_nr_active_zones - 1;

  if (info.max_nr_open_zones == 0)
    max_nr_open_io_zones_ = info.nr_zones;
  else
    max_nr_open_io_zones_ = info.max_nr_open_zones - 1;
  
  Info(logger_, "Zone block device nr zones: %u max active: %u max open: %u \n",
       info.nr_zones, info.max_nr_active_zones, info.max_nr_open_zones);

  addr_space_sz = (uint64_t)nr_zones_ * zone_sz_;

  ret = zbd_list_zones(read_f_, 0, addr_space_sz, ZBD_RO_ALL, &zone_rep,
                       &reported_zones);

  if (ret || reported_zones != nr_zones_) {
    Error(logger_, "Failed to list zones, err: %d", ret);
    return IOStatus::IOError("Failed to list zones");
  }

  while (m < ZENFS_META_ZONES && i < reported_zones) {
    struct zbd_zone *z = &zone_rep[i++];
    /* Only use sequential write required zones */
    if (zbd_zone_type(z) == ZBD_ZONE_TYPE_SWR) {
      if (!zbd_zone_offline(z)) {
        Zone* new_zone = new Zone(this, z, zone_cnt);
        meta_zones.push_back(new_zone);
        id_to_zone_.insert(std::pair<int,Zone*>(zone_cnt, new_zone));
        zone_cnt++;
      }
      m++;
    }
  }
 
  //(TODO)::Should reserved zone be treated as active_io_zones_?
  while(r <= RESERVED_ZONE_FOR_CLEANING && i < reported_zones) {
    struct zbd_zone *z = &zone_rep[i++];
    /* Only use sequential write required zones */
    if (zbd_zone_type(z) == ZBD_ZONE_TYPE_SWR) {
      if (!zbd_zone_offline(z)) {
        Zone* new_zone = new Zone(this, z, zone_cnt);
        reserved_zones.push_back(new_zone);
        id_to_zone_.insert(std::pair<int,Zone*>(zone_cnt, new_zone));
        zone_cnt++;
      }
      r++;
    }
  }
    
  active_io_zones_ = 0;
  open_io_zones_ = 0;

  for (; i < reported_zones; i++) {
    struct zbd_zone *z = &zone_rep[i];
    /* Only use sequential write required zones */
    if (zbd_zone_type(z) == ZBD_ZONE_TYPE_SWR) {
      if (!zbd_zone_offline(z)) {
        Zone *newZone = new Zone(this, z, zone_cnt);
        io_zones.push_back(newZone);
        id_to_zone_.insert(std::pair<int,Zone*>(zone_cnt, newZone));
        zone_cnt++;

        if (zbd_zone_imp_open(z) || zbd_zone_exp_open(z) ||
            zbd_zone_closed(z)) {
          active_io_zones_++;
          if (zbd_zone_imp_open(z) || zbd_zone_exp_open(z)) {
            if (!readonly) {
              newZone->Close();
            }
          }
        }
      }
    }
  }

  free(zone_rep);
  start_time_ = time(NULL);

  return IOStatus::OK();
}

void ZonedBlockDevice::NotifyIOZoneFull() {
  const std::lock_guard<std::mutex> lock(zone_resources_mtx_);
  active_io_zones_--;
  zone_resources_.notify_one();
}

void ZonedBlockDevice::NotifyIOZoneClosed() {
  const std::lock_guard<std::mutex> lock(zone_resources_mtx_);
  open_io_zones_--;
  zone_resources_.notify_one();
}


void ZonedBlockDevice::LogZoneStats() {
  uint64_t used_capacity = 0;
  uint64_t reclaimable_capacity = 0;
  uint64_t reclaimables_max_capacity = 0;
  uint64_t active = 0;
  io_zones_mtx.lock();

  for (const auto z : io_zones) {
    used_capacity += z->used_capacity_;

    if (z->used_capacity_) {
      reclaimable_capacity += z->max_capacity_ - z->used_capacity_;
      reclaimables_max_capacity += z->max_capacity_;
    }

    if (!(z->IsFull() || z->IsEmpty())) active++;
  }

  if (reclaimables_max_capacity == 0) reclaimables_max_capacity = 1;

  Info(logger_,
       "[Zonestats:time(s),used_cap(MB),reclaimable_cap(MB), "
       "avg_reclaimable(%%), active(#), active_zones(#), open_zones(#)] %ld "
       "%lu %lu %lu %lu %ld %ld\n",
       time(NULL) - start_time_, used_capacity / MB, reclaimable_capacity / MB,
       100 * reclaimable_capacity / reclaimables_max_capacity, active,
       active_io_zones_.load(), open_io_zones_.load());

  io_zones_mtx.unlock();
}

void ZonedBlockDevice::LogZoneUsage() {
  for (const auto z : io_zones) {
    int64_t used = z->used_capacity_;

    if (used > 0) {
      Debug(logger_, "Zone 0x%lX used capacity: %ld bytes (%ld MB)\n",
            z->start_, used, used / MB);
    }
  }
}

ZonedBlockDevice::~ZonedBlockDevice() {
  
  for (const auto z : meta_zones) {
    delete z;
  }

  for (const auto z : io_zones) {
    delete z;
  }
  zbd_close(read_f_);
  zbd_close(read_direct_f_);
  zbd_close(write_f_);
}

#define LIFETIME_DIFF_NOT_GOOD (100)

unsigned int GetLifeTimeDiff(Env::WriteLifeTimeHint zone_lifetime,
                             Env::WriteLifeTimeHint file_lifetime) {
  assert(file_lifetime <= Env::WLTH_EXTREME);

  if ((file_lifetime == Env::WLTH_NOT_SET) ||
      (file_lifetime == Env::WLTH_NONE)) {
    if (file_lifetime == zone_lifetime) {
      return 0;
    } else {
      return LIFETIME_DIFF_NOT_GOOD;
    }
  }

  if (zone_lifetime > file_lifetime) return zone_lifetime - file_lifetime;

  return LIFETIME_DIFF_NOT_GOOD;
}

double GetSLifeTimeDiff(const Zone* zone, double zone_secondary_lifetime,
                             Env::WriteLifeTimeHint file_lifetime) {
  assert(file_lifetime <= Env::WLTH_EXTREME);
  uint64_t total_length = 0;
  uint64_t expected_length = 0;
  double slt = 0;

  for(const auto e : zone->extent_info_) {
      total_length += e->length_;
  }

  expected_length = (total_length / (zone->extent_info_.size()));
  total_length += expected_length;
  
  for(const auto e : zone->extent_info_) {
      double weigth = (((double)e->length_)/(total_length));
      slt += (weigth * (double)(e->lt_));
  }
  
  double w = (((double)expected_length)/(total_length));
  slt += (w * (double)(file_lifetime));

  if (zone_secondary_lifetime >= slt){
    return zone_secondary_lifetime - slt;
  }    
  
  return slt - zone_secondary_lifetime;
}

Zone *ZonedBlockDevice::AllocateMetaZone() {
  for (const auto z : meta_zones) {
    /* If the zone is not used, reset and use it */
    if (!z->IsUsed()) {
      if (!z->IsEmpty()) {
        if (!z->Reset().ok()) {
          Warn(logger_, "Failed resetting zone!");
          continue;
        }
      }
      return z;
    }
  }
  return nullptr;
}

void ZonedBlockDevice::ResetUnusedIOZones() {
  const std::lock_guard<std::mutex> lock(zone_resources_mtx_);
  /* Reset any unused zones */
  for (const auto z : io_zones) {
    if (!z->IsUsed() && !z->IsEmpty()) {
      if (!z->IsFull()) active_io_zones_--;
      if (!z->Reset().ok()) Warn(logger_, "Failed reseting zone");
    }
  }
}
/*(TODO)
void ZonedBlockDevice::PickZoneWithCompactionVictim(std::vector<Zone*>& candidates) {
 io_zones_mtx should be locked before the function is called 

    std::vector<uint64_t> fno_list;
    db_ptr_->GetCompactionArgs(fno_list);
}
*/
void ZonedBlockDevice::PickZoneWithOnlyInvalid(std::vector<Zone*>& candidates) {
/* io_zones_mtx should be locked before the function is called */
    for (const auto z : io_zones) {
       if ((!z->IsUsed()) && (!z->IsEmpty()) && (!z->IsFull()) ) {
         candidates.push_back(z);
       }
    }
}
void ZonedBlockDevice::SortZone(){

  while (!allocate_queue_.empty()) {
     AllocVictimZone* a = allocate_queue_.top();
     allocate_queue_.pop();
     delete a;
  }
  for (const auto z : io_zones) {
   if(z->IsFull() || z->open_for_write_) continue;

   uint64_t valid_extent_length = 0;
   uint64_t invalid_extent_length = 0;
   
   for (auto ext_info: z->extent_info_) {
    if (ext_info->valid_) {
      uint64_t cur_length = (uint64_t)ext_info->length_;
      uint64_t align = (uint64_t)(cur_length % block_sz_);
      uint64_t pad = 0;
      if (align) {
        pad = block_sz_ - align;
      }
      valid_extent_length += (cur_length + pad);
    } else {
      uint64_t cur_length = (uint64_t)ext_info->length_;
      uint64_t align = (uint64_t)(cur_length % block_sz_);
      uint64_t pad = 0;
      if (align) {
        pad = block_sz_ - align;
      }
      invalid_extent_length += (cur_length + pad);
    }
   }
   //Insert into queue with sorting by its invalid ratio. 
   ///Higher the invalid ratio, Higher the priority.
   allocate_queue_.push(new AllocVictimZone(z, invalid_extent_length, valid_extent_length));
 }
 //assert(!allocate_queue_.empty());
};

Zone* ZonedBlockDevice::AllocateZoneWithSameLevelFiles(const std::vector<uint64_t>& fno_list, const InternalKey smallest, const InternalKey largest) {
   
    Zone* allocated_zone = nullptr;
    const InternalKeyComparator* icmp = db_ptr_->GetDefaultICMP();
    int l_idx, r_idx;


    if (fno_list.empty()) {//û��ͬ���ļ������
      return allocated_zone;
    }


    if (fno_list.size() == 1) {//ֻ��һ��ͬ���ļ������
     sst_zone_mtx_.lock();
     auto zids = sst_to_zone_.find(fno_list[0])->second;//���ļ��Ų�������ļ��Ŷ�Ӧ��zone��
     for (int zid : zids) {
       Zone* z = id_to_zone_.find(zid)->second;//ͨ��zone���ҵ�zone
       if (!z->open_for_write_ && !z->IsFull() && !z->open_for_write_) {
         allocated_zone = z;//û��Ҳ����д���ͷ����zone
       }
     }
     sst_zone_mtx_.unlock();
     return allocated_zone;
    }

    //Find the case.
    //case(1) cur SSTable has smallest key.
    //case(2) cur SSTable has largest key.
    //case(3) cur SSTable has middle key.

    int idx;
    files_mtx_.lock();
    for (idx = 0; idx < (int)fno_list.size(); idx++) {//�����ļ��б�
        InternalKey s, l;
        s = files_.find(fno_list[idx])->second->smallest_;//ͨ���ļ��ű���zonefile���õ���С������
        l = files_.find(fno_list[idx])->second->largest_;
        int res = icmp->Compare(largest,s);
 
        if (res <= 0) {
          assert(icmp->Compare(smallest,l) <= 0);
          break;
        }
    }
    files_mtx_.unlock();
    l_idx = idx - 1;
    r_idx = idx;

    if ( l_idx < 0 ) { // case(1)��д���ļ���ͬ�㼶�ļ���key��С
     for (uint64_t fno : fno_list) {
      sst_zone_mtx_.lock();
      auto zids = sst_to_zone_.find(fno)->second;
      for (int zid : zids) {
       Zone* z = id_to_zone_.find(zid)->second;
       if (!z->open_for_write_ && !z->IsFull()) {
         allocated_zone = z;//�ҵ�һ����д���ͬ���ļ���д
         break;
       }
      }
      sst_zone_mtx_.unlock();
      if (allocated_zone) break;
     }   
    } else if (r_idx == (int)fno_list.size()) { // case(2)
     for (auto it = fno_list.rbegin(); it != fno_list.rend(); ++it) {//�������list
       sst_zone_mtx_.lock();
       auto zids = sst_to_zone_.find(*it)->second;
       for (int zid : zids) {
         Zone* z = id_to_zone_.find(zid)->second;
         if (!z->open_for_write_ && !z->IsFull()) {
           allocated_zone = z;
           break;
         }
       }
       sst_zone_mtx_.unlock();
       if (allocated_zone) break;
     }       
    } else {
     while ((l_idx >=0) && (r_idx < (int)fno_list.size())) {
       if (l_idx >=0) {
         sst_zone_mtx_.lock();
         auto zids = sst_to_zone_.find(l_idx)->second;
         for (int zid : zids) {
           Zone* z = id_to_zone_.find(zid)->second;
           if (!z->open_for_write_ && !z->IsFull()) {
            allocated_zone = z;
            break;
           }
         }
         sst_zone_mtx_.unlock();
         l_idx--;
         if(allocated_zone) break;
       }
       if (r_idx < (int)fno_list.size()) {
         sst_zone_mtx_.lock();
         auto zids = sst_to_zone_.find(r_idx)->second;
         for (int zid : zids) {
           Zone* z = id_to_zone_.find(zid)->second;
           if (!z->open_for_write_ && !z->IsFull()) {
            allocated_zone = z;
            break;
           }
         }
         sst_zone_mtx_.unlock();
         r_idx++;
         if (allocated_zone) break;
       }
     }
    }
    return allocated_zone;
}

Zone* ZonedBlockDevice::AllocateMostL0Files(const std::set<int>& zone_list) {

    uint64_t max = 0;
    Zone* z = nullptr;

    if (zone_list.empty()) 
      return z;

    for (const auto z_id : zone_list) {
      Zone* zone = id_to_zone_.find(z_id)->second;
      uint64_t length = 0;
      
      if (!zone->open_for_write_ && !zone->IsFull()) {
        for (const auto& ext : zone->extent_info_) {
          if (ext->level_ == 0 && ext->valid_) {
              length += ext->length_;    
          }
        }
        if (length >= max){ 
          max = length;
          z = zone;
        }
      }
    }
    return z;
}

void ZonedBlockDevice::SameLevelFileList(const int level, std::vector<uint64_t>& fno_list){
    fno_list.clear();
    db_ptr_->SameLevelFileList(level, fno_list);
}

void ZonedBlockDevice::AdjacentFileList(const InternalKey& s, const InternalKey& l, const int level, std::vector<uint64_t>& fno_list){
    if(level == 100) return;
    db_ptr_->AdjacentFileList(s, l, level, fno_list);
}
void ZonedBlockDevice::AllFile(const InternalKey& s, const InternalKey& l,std::vector<uint64_t>& fno_list) {//��ȡȫ���㼶���ļ���
   fno_list.clear();
   int levelnum = db_ptr_->Getlevel();
   for (int level = 0; level < levelnum; ++level) {
     std::vector<uint64_t> temp_list;
     SameLevelFileList(level, temp_list);
     fno_list.insert(fno_list.end(), temp_list.begin(), temp_list.end());
    }
}
uint64_t ZonedBlockDevice::hexToUint64(const std::string& hex) {
  uint64_t result = 0;
  std::stringstream ss;
  ss << std::hex << hex;
  ss >> result;
  return result;
}
uint64_t ZonedBlockDevice::HexString(ParsedInternalKey* result) {
  std::string newstring = result.UserkeyString();
  return hexToUint64(newstring);


}
Zone* ZonedBlockDevice::AllocateZone(Env::WriteLifeTimeHint file_lifetime,
                                     InternalKey smallest, InternalKey largest,
                                     int level) {

  Zone *allocated_zone = nullptr;
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  Status s;
  
  io_zones_mtx.lock();
  /* Make sure we are below the zone open limit */
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    zone_resources_.wait(lk, [this] {
      if (open_io_zones_.load() < max_nr_open_io_zones_) return true;
      return false;
    });
  }
  
  /* Sort Zone by follows rules
   * (1) has more valid data
   * (2) has less invalid data */
  SortZone();
  /* Reset any unused zones and finish used zones under capacity treshold*/
  for (const auto z : io_zones) {
    if (z->open_for_write_ || z->IsEmpty() || (z->IsFull() && z->IsUsed()))
      continue;
    
    if (!z->IsUsed())  {
      if (!z->IsFull()) active_io_zones_--;
      bool all_invalid = true;
      
      for (auto exinfo : z->extent_info_) {
        if (exinfo->valid_ == true) {
          all_invalid = false;
        }
      }
      assert(all_invalid);
      s = z->Reset();

      if (!s.ok()) {
        Debug(logger_, "Failed resetting zone !");
      }
      continue;
    }
    
    if ((z->capacity_ < (z->max_capacity_ * finish_threshold_ / 100))) {
      /* If there is less than finish_threshold_% remaining capacity in a
       * non-open-zone, finish the zone */
      s = z->Finish();
      if (!s.ok()) {
        Debug(logger_, "Failed finishing zone");
      }
      active_io_zones_--;
    }
  }
#ifndef LAZY
  {
    uint64_t free = GetFreeSpace();
    size_t nr_zones = io_zones.size();
    uint64_t total = (nr_zones * io_zones[0]->max_capacity_);
    //fprintf(stderr, "total : %zu , free : %zu\n", total, free);
    double free_ratio = (((double)free / total) * 100);

    bool trigger_zc = free_ratio <= 25.0f;
   
    if (trigger_zc) {
      uint64_t num_zone_to_reset;
      if (free_ratio > 25.0f) {
          num_zone_to_reset = nr_zones / 15;   
      } else if (free_ratio > 20.0f) {
          num_zone_to_reset = nr_zones / 10;       
      } else {
          num_zone_to_reset = nr_zones / 5;       
      }
      while (!gc_queue_.empty()){
        auto a = gc_queue_.top();
        delete a;
        gc_queue_.pop();
      }
    for (auto z : io_zones) {
        uint64_t valid_extent_length = 0;
        uint64_t invalid_extent_length = 0;
        std::vector<ZoneExtentInfo *> invalid_list;
        std::vector<ZoneExtentInfo *> valid_list;
        for(auto ext_info: z->extent_info_) {
          
          //    Busy wait til Append request to the zone completed.
          //    No need to check the condition in the loop 
          //    since zone is allocated to one file at each time.
            
            while(z->is_append.load()){ }
            
            if(ext_info->valid_) {
                uint64_t cur_length = (uint64_t)ext_info->length_;
                uint64_t align = (uint64_t)(cur_length % block_sz_);
                uint64_t pad = 0;
                if(align){
                    pad = block_sz_ - align;
                }
                valid_extent_length += (cur_length + pad);
                valid_list.push_back(ext_info);
            }else {
                uint64_t cur_length = (uint64_t)ext_info->length_;
                uint64_t align = (uint64_t)(cur_length % block_sz_);
                uint64_t pad = 0;
                if(align){
                    pad = block_sz_ - align;
                }
                invalid_extent_length += (cur_length + pad);
                invalid_list.push_back(ext_info);
            }
        }
        //Insert into queue with sorting by its invalid ratio. 
        //Higher the invalid ratio, Higher the priority.
        if (invalid_extent_length > 0 && !z->open_for_write_) {
          gc_queue_.push(new GCVictimZone(z, invalid_extent_length));
        }
    }
   ZoneCleaning(num_zone_to_reset);
  }
 }
#endif

  if (sst_to_zone_.empty()) {//���û��sst��zone��
    if (active_io_zones_.load() < max_nr_active_io_zones_) {
      for (const auto z : io_zones) {//ֱ����һ���յ�zone��ԭ������д��zone���з���
        if ((!z->open_for_write_) && z->IsEmpty()) {
          z->lifetime_ = file_lifetime;
          allocated_zone = z;
          active_io_zones_++;
          break;
        }
      }
    }
  }
  if (allocated_zone) {
    assert(!allocated_zone->open_for_write_);
    allocated_zone->open_for_write_ = true;
    open_io_zones_++;
    io_zones_mtx.unlock();
    return allocated_zone;
  }
  assert(!allocated_zone);

  // There's valid SSTables in Zones
  // Find zone where the files located at adjacent level and having overlapping keys
  std::vector<uint64_t> fno_list;
  std::vector<Zone*> candidates;
  //AdjacentFileList(smallest, largest, level, fno_list);//fnolist�õ��������²㼶���������ص���list
  GetAllOverlappingFiles(smallest, largest,fno_list)//��ȡ���в������ص����ļ�
      if (!fno_list.empty()) {
    std::vector<std::pair<uint64_t, double>> overlap_ratios;
    double maxoverlap = 0.0
    int idx;
    files_mtx_.lock();
    for (idx = 0; idx < (int)fno_list.size(); idx++) {
      InternalKey s, l,max,min,overmin,overmax;
      s = files_.find(fno_list[idx])->second->smallest_;
      l = files_.find(fno_list[idx])->second->largest_;
      if (icmp->Compare(l, largest) <= 0) {
        max = largest;
        overmax = l;
      } else {
        max = l;
        overmax = largest;
      }
      if (icmp->Compare(s, smallest) <= 0) {
        min = s;
        overmin = smallest;
      } else {
        min = smallest;
        overmin = s;
      }
      ParsedInternalKey fullmax, fullmin, fullovermax, fullovermin;
      ParseInternalKey(max, &fullmax);
      ParseInternalKey(min, &fullmin);
      ParseInternalKey(overmax, &fullovermax);
      ParseInternalKey(overmin, &fullovermin);
      uint64_t num_max, num_min, num_overmax, num_overmin;
      num_max = HexString(fullmax);
      num_min = HexString(fullmin);
      num_overmax = HexString(fullovermax);
      num_overmin = HexString(fullovermin);
      double radio = (num_overmax - num_overmin) / (numm_max - num_min);
      overlap_ratios.emplace_back(fno_list[idx], ratio);
      
    }
    files_mtx_.unlock();

    // �������ʽ�������
    std::sort(overlap_ratios.begin(), overlap_ratios.end(),
              [](const std::pair<uint64_t, double>& a,
                 const std::pair<uint64_t, double>& b) {
                return a.second > b.second;
              });

    // ���� fno_list ˳��
    fno_list.clear();
    for (const auto& entry : overlap_ratios) {
      fno_list.push_back(entry.first);
    }
      }
   if (!fno_list.empty()) {  //�ص��ļ��б�Ϊ��
    // There are SSTables with overlapped keys and adjacent level.
    // (1) Find the Zone where the SSTables are written
    std::set<int> zone_list;
    sst_zone_mtx_.lock();
    for (uint64_t fno : fno_list) {
      auto z = sst_to_zone_.find(fno);
      if (z != sst_to_zone_.end()) {
        for (int zone_id : z->second) {
          zone_list.insert(zone_id);  //������zone�ᵽzonelist��
        }
      }
    }
    sst_zone_mtx_.unlock();

    // (2) Pick the Zones with free space as candidates
    for (const auto z : io_zones) {
      auto search = zone_list.find(z->zone_id_);
      if (search != zone_list.end()) {
        if (!z->IsFull() && !z->open_for_write_) {
          candidates.push_back(z);  //������zone��Ϊ��ѡ
        }
      }
    } 
     
    if (!candidates.empty()) {
      uint64_t alloc_inval_data = 0;
      double maxoverlap_ratio = 0;
      for (const auto z : candidates) {
        uint64_t inval_data = 0;
        for (const auto ext :z->extent_info_) {  //����ÿһ��_zone extent�������
          if (!ext->valid_) {
            inval_data += ext->length_;
          } 
            }
        if (!z->open_for_write_) {
          allocated_zone = z;
          alloc_inval_data = inval_data;
          break;
          }
          
          }
          }
        
      
  } else if (fno_list.empty() && (level==0 || level==100 )) {

   /* (1) There is no matching files being overlapped with current file
        ->(TODO)Find the file within the same level which has smallest key diff*/
    std::set<int> zone_list;
    // L0 files are often compacted altogether.
    // Allocate to the zone where other L0 or L1 files are located.
    SameLevelFileList(0, fno_list);

    sst_zone_mtx_.lock();
    for (uint64_t fno : fno_list) {
      auto z = sst_to_zone_.find(fno);
      if (z != sst_to_zone_.end()) {
        for (int zone_id : z->second) {
          zone_list.insert(zone_id);
        }
      }
    }
    sst_zone_mtx_.unlock();
    //Allocate Zones with most the number of L0 files
    allocated_zone = AllocateMostL0Files(zone_list);
  }

  //Find the Empty Zone First
  if (!allocated_zone) {
    if (active_io_zones_.load() < max_nr_active_io_zones_) {
      for (const auto z : io_zones) {
       if ((!z->open_for_write_) && z->IsEmpty()) {
        z->lifetime_ = file_lifetime;
        allocated_zone = z;
        active_io_zones_++;
        break;
       }
      }
    }
  }

  if (allocated_zone){
    assert(!allocated_zone->open_for_write_);
    allocated_zone->open_for_write_ = true;
    open_io_zones_++;
    io_zones_mtx.unlock();
    LogZoneStats();
    return allocated_zone;
  }

  if (!allocated_zone) {
    SameLevelFileList(level, fno_list);
    allocated_zone = AllocateZoneWithSameLevelFiles(fno_list, smallest, largest);
  }

  if (allocated_zone) {
    assert(!allocated_zone->open_for_write_);
    allocated_zone->open_for_write_ = true;
    open_io_zones_++;
    io_zones_mtx.unlock();
    return allocated_zone;
  }
  
  /* Try to fill an already open zone(with the best life time diff) */
  for (const auto z : io_zones) {
    if ((!z->open_for_write_) && (z->used_capacity_ > 0) && !z->IsFull()) {
      unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
      if (diff <= best_diff) {
        allocated_zone = z;
        best_diff = diff;
      }
    }
  }

  if (allocated_zone) {
    assert(!allocated_zone->open_for_write_);
    allocated_zone->open_for_write_ = true;
    open_io_zones_++;
    io_zones_mtx.unlock();
    return allocated_zone;
  }

#ifndef LAZY
  if (!allocated_zone) {
   while (!gc_queue_.empty()){
      auto a = gc_queue_.top();
      delete a;
      gc_queue_.pop();
   }
  //Trigger GC for reclaim free space in the Device.
  //(Step 1) Classify all active zones by its invalid data ratio.
    uint64_t total_invalid = 0;
    for (auto z : io_zones) {
        uint64_t valid_extent_length = 0;
        uint64_t invalid_extent_length = 0;
        std::vector<ZoneExtentInfo *> invalid_list;
        std::vector<ZoneExtentInfo *> valid_list;
        for(auto ext_info: z->extent_info_) {
            /*
              Busy wait til Append request to the zone completed.
              No need to check the condition in the loop 
              since zone is allocated to one file at each time.
            */
            while(z->is_append.load()){ }
            
            if(ext_info->valid_) {
                uint64_t cur_length = (uint64_t)ext_info->length_;
                uint64_t align = (uint64_t)(cur_length % block_sz_);
                uint64_t pad = 0;
                if(align){
                    pad = block_sz_ - align;
                }
                valid_extent_length += (cur_length + pad);
                valid_list.push_back(ext_info);
            }else {
                uint64_t cur_length = (uint64_t)ext_info->length_;
                uint64_t align = (uint64_t)(cur_length % block_sz_);
                uint64_t pad = 0;
                if(align){
                    pad = block_sz_ - align;
                }
                invalid_extent_length += (cur_length + pad);
                total_invalid += invalid_extent_length;
                invalid_list.push_back(ext_info);
            }
        }
        //Insert into queue with sorting by its invalid ratio. 
        //Higher the invalid ratio, Higher the priority.
        if (invalid_extent_length > 0 && !z->open_for_write_) {
          gc_queue_.push(new GCVictimZone(z, invalid_extent_length));
        }
    }
  uint64_t num_zone_to_reset;
  if (total_invalid  < io_zones[0]->max_capacity_ ){
    num_zone_to_reset = 0;
  } else {
    num_zone_to_reset = RESERVED_ZONE_FOR_CLEANING;
  }
  ZoneCleaning(num_zone_to_reset);
  }

  fno_list.clear();
  candidates.clear();
  //AdjacentFileList(smallest, largest, level, fno_list);
  GetAllOverlappingFiles(smallest, largest,
                         fno_list)  //��ȡ���в������ص����ļ�
      if (!fno_list.empty()) {
    std::vector<std::pair<uint64_t, double>> overlap_ratios;
    double maxoverlap = 0.0 int idx;
    files_mtx_.lock();
    for (idx = 0; idx < (int)fno_list.size(); idx++) {
      InternalKey s, l, max, min, overmin, overmax;
      s = files_.find(fno_list[idx])->second->smallest_;
      l = files_.find(fno_list[idx])->second->largest_;
      if (icmp->Compare(l, largest) <= 0) {
        max = largest;
        overmax = l;
      } else {
        max = l;
        overmax = largest;
      }
      if (icmp->Compare(s, smallest) <= 0) {
        min = s;
        overmin = smallest;
      } else {
        min = smallest;
        overmin = s;
      }
      ParsedInternalKey fullmax, fullmin, fullovermax, fullovermin;
      ParseInternalKey(max, &fullmax);
      ParseInternalKey(min, &fullmin);
      ParseInternalKey(overmax, &fullovermax);
      ParseInternalKey(overmin, &fullovermin);
      uint64_t num_max, num_min, num_overmax, num_overmin;
      num_max = HexString(fullmax);
      num_min = HexString(fullmin);
      num_overmax = HexString(fullovermax);
      num_overmin = HexString(fullovermin);
      double radio = (num_overmax - num_overmin) / (numm_max - num_min);
      overlap_ratios.emplace_back(fno_list[idx], ratio);
    }
    files_mtx_.unlock();

    // �������ʽ�������
    std::sort(overlap_ratios.begin(), overlap_ratios.end(),
              [](const std::pair<uint64_t, double>& a,
                 const std::pair<uint64_t, double>& b) {
                return a.second > b.second;
              });

    // ���� fno_list ˳��
    fno_list.clear();
    for (const auto& entry : overlap_ratios) {
      fno_list.push_back(entry.first);
    }
  }
  if (!fno_list.empty()) {  //�ص��ļ��б�Ϊ��
    // There are SSTables with overlapped keys and adjacent level.
    // (1) Find the Zone where the SSTables are written
    std::set<int> zone_list;
    sst_zone_mtx_.lock();
    for (uint64_t fno : fno_list) {
      auto z = sst_to_zone_.find(fno);
      if (z != sst_to_zone_.end()) {
        for (int zone_id : z->second) {
          zone_list.insert(zone_id);  //������zone�ᵽzonelist��
        }
      }
    }
    sst_zone_mtx_.unlock();

    // (2) Pick the Zones with free space as candidates
    for (const auto z : io_zones) {
      auto search = zone_list.find(z->zone_id_);
      if (search != zone_list.end()) {
        if (!z->IsFull() && !z->open_for_write_) {
          candidates.push_back(z);  //������zone��Ϊ��ѡ
        }
      }
    }

    if (!candidates.empty()) {
      uint64_t alloc_inval_data = 0;
      double maxoverlap_ratio = 0;
      for (const auto z : candidates) {
        uint64_t inval_data = 0;
        for (const auto ext :
             z->extent_info_) {  //����ÿһ��_zone extent�������
          if (!ext->valid_) {
            inval_data += ext->length_;
          }
        }
        if (!z->open_for_write_) {
          allocated_zone = z;
          alloc_inval_data = inval_data;
          break;
        }
      }
    }
  } 
  /* if (!fno_list.empty()) {
   // There are SSTables with overlapped keys and adjacent level.
      //(1) Find the Zone where the SSTables are written 
    std::set<int> zone_list;
    sst_zone_mtx_.lock();
    for (uint64_t fno : fno_list) {
      auto z = sst_to_zone_.find(fno);
      if (z != sst_to_zone_.end()) {
        for (int zone_id : z->second) {
          zone_list.insert(zone_id);
        }
      }
    }
    sst_zone_mtx_.unlock();
    // (2) Pick the Zones with free space as candidates
    for (const auto z : io_zones) {
      auto search = zone_list.find(z->zone_id_);
      if (search != zone_list.end()){
        if (!z->IsFull() && !z->open_for_write_) {
          candidates.push_back(z);
        }
      }
    }
    if (!candidates.empty()) {

      // There is at least one zone having free space 
      // If there's more than one zone, pick the zone with most data with overlapped SST
      uint64_t overlapped_data = 0;
      uint64_t alloc_inval_data = 0;
      for (const auto z : candidates) {
        uint64_t data_amount = 0;
        uint64_t inval_data = 0;
        for (const auto ext : z->extent_info_) {
           if (!ext->valid_) {
             inval_data += ext->length_;
           } else if (ext->valid_ && ext->zone_file_->is_sst_) {
             uint64_t cur_fno = ext->zone_file_->fno_;
             for (uint64_t fno : fno_list) {
                if (cur_fno == fno ) {
                   data_amount += ext->length_; 
                }
             }
           }
           if (data_amount > overlapped_data && !z->open_for_write_) {
             allocated_zone = z;
             alloc_inval_data = inval_data;
           } else if ((data_amount == overlapped_data) && (alloc_inval_data > inval_data) && !z->open_for_write_ ) {
             allocated_zone = z;
             alloc_inval_data = inval_data;
           }
        }
      }
    }  
  }*/ else if (fno_list.empty() && (level == 0 || level==100)) {
    /* (1) There is no matching files being overlapped with current file
        ->(TODO)Find the file within the same level which has smallest key diff*/
    std::set<int> zone_list;
    // L0 files are often compacted altogether.
    // Allocate to the zone where other L0 or L1 files are located.
    SameLevelFileList(0, fno_list);
    sst_zone_mtx_.lock();
    for (uint64_t fno : fno_list) {
      auto z = sst_to_zone_.find(fno);
      if (z != sst_to_zone_.end()) {
        for (int zone_id : z->second) {
          zone_list.insert(zone_id);
        }
      }
    }
    sst_zone_mtx_.unlock();
    //Allocate Zones with most the number of L0 files
    allocated_zone = AllocateMostL0Files(zone_list);
  }
  //Find the Empty Zone First
  if (!allocated_zone) {
    if (active_io_zones_.load() < max_nr_active_io_zones_) {
      for (const auto z : io_zones) {
       if ((!z->open_for_write_) && z->IsEmpty()) {
        z->lifetime_ = file_lifetime;
        allocated_zone = z;
        active_io_zones_++;
        break;
       }
      }
    }
  }
  if (allocated_zone){
    assert(!allocated_zone->open_for_write_);
    allocated_zone->open_for_write_ = true;
    open_io_zones_++;
    io_zones_mtx.unlock();
    return allocated_zone;
  }
  
  if (!allocated_zone && level != 100) {
    SameLevelFileList(level, fno_list);
    allocated_zone = AllocateZoneWithSameLevelFiles(fno_list, smallest, largest);
  }

  if (allocated_zone) {
    assert(!allocated_zone->open_for_write_);
    allocated_zone->open_for_write_ = true;
    open_io_zones_++;
    io_zones_mtx.unlock();
    return allocated_zone;
  }
  
  /* Try to fill an already open zone(with the best life time diff) */
  for (const auto z : io_zones) {
    if ((!z->open_for_write_) && (z->used_capacity_ > 0) && !z->IsFull()) {
      unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
      if (diff <= best_diff) {
        allocated_zone = z;
        best_diff = diff;
      }
    }
  }

  if (allocated_zone) {
    assert(!allocated_zone->open_for_write_);
    allocated_zone->open_for_write_ = true;
    open_io_zones_++;
    io_zones_mtx.unlock();
    return allocated_zone;
  }
#endif
  io_zones_mtx.unlock();
  LogZoneStats();

  return allocated_zone;
}

std::string ZonedBlockDevice::GetFilename() { return filename_; }
uint32_t ZonedBlockDevice::GetBlockSize() { return block_sz_; }

Zone *ZonedBlockDevice::AllocateZoneForCleaning() {

  Zone *allocated_zone = nullptr;
  Status s;

  /* Make sure we are below the zone open limit */
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    zone_resources_.wait(lk, [this] {
      if (open_io_zones_.load() < max_nr_open_io_zones_) return true;
      return false;
    });
  }

  allocated_zone = reserved_zones[0];

  if (!allocated_zone) {
      printZoneStatus(reserved_zones);
      fprintf(stderr, "Allocate Zone Failed While Running Zone Cleaning!\n");
      exit(1);
  }
  assert(!allocated_zone->open_for_write_);
  allocated_zone->open_for_write_ = true;
  open_io_zones_++;

  return allocated_zone;
}

void ZonedBlockDevice::printZoneStatus(const std::vector<Zone *>& zones){

    for (auto z : zones) {
        
        fprintf(stderr, "start : %ld\n", z->start_);
        fprintf(stderr, "wp_ : %ld\n", z->wp_);
        fprintf(stderr, "capacity_ : %ld\n", z->capacity_);
        fprintf(stderr, "used_capacity_ : %ld\n", z->used_capacity_.load());

        if(z->open_for_write_) {
            fprintf(stderr, "open_for_write_ : true\n");
        }else{
            fprintf(stderr, "open_for_write_ : false\n");
        }
        
        if(z->IsUsed()) {
            fprintf(stderr, "is_used : true\n");
        }else{
            fprintf(stderr, "is_used : false\n");
        }

        if(z->IsFull()) {
            fprintf(stderr, "is_full : true\n");
        }else{
            fprintf(stderr, "is_full : false\n");
        }

        if(z->IsEmpty()) {
            fprintf(stderr, "is_empty : true\n");
        }else{
            fprintf(stderr, "is_empty : false\n");
        }
        fprintf(stderr, "\n\n");
    }
}

/*
 ZoneCleaning
 (1) Select zone with most invalid data.
 (2) Process until every invalid data gets cleaned from zone.
*/
int ZonedBlockDevice::ZoneCleaning(int nr_reset) {

    zone_cleaning_mtx.lock();
    int reseted = 0;

    if (nr_reset == 0){
       for (auto it = reserved_zones.begin(); it != reserved_zones.end(); ){
          io_zones.push_back(*it);
          reserved_zones.erase(it);
          break;
       }
      zone_cleaning_mtx.unlock();
      return 0;
    }

#ifdef EXPERIMENT
    uint64_t copied_data = 0;
#endif
    Zone* allocated_zone = nullptr;
    while(!gc_queue_.empty()){
        //Process until every invalid data gets cleaned from zone.
        Zone* cur_victim = gc_queue_.top()->get_zone_ptr();
        int victim_zone_id = cur_victim->zone_id_;
        assert(cur_victim);

        //PrintVictimInformation(cur_victim, true);

        //Find the valid extents in currently selected zone.
        //Should recognize which file each extent belongs to.
        std::vector<ZoneExtentInfo *> valid_extents_info;

        for (auto exinfo : cur_victim->extent_info_){
           if (exinfo->valid_ == true) {
             valid_extents_info.push_back(exinfo);
           }
        }
        
        //(1) Find which ZoneFile current extents belongs to.
        //(2) Check Each lifetime of file to which each extent belongs to.    
        for(ZoneExtentInfo* ext_info : valid_extents_info) {
            //Extract All the inforamtion from Extents inforamtion structure
            assert(cur_victim == ext_info->extent_->zone_);
            ZoneExtent* zone_extent = ext_info->extent_;
            ZoneFile* zone_file = ext_info->zone_file_;
            
            zone_file->ExtentWriteLock();

            assert(zone_extent && zone_file);

            //extract the contents of the current extent
            uint32_t valid_size = zone_extent->length_; 
            uint32_t data_size = valid_size;
            uint32_t pad_sz = 0;
            uint32_t align = valid_size % block_sz_;

            if (align) {
              uint32_t block_nr = (valid_size / block_sz_) + 1;
              data_size = block_sz_ * block_nr;
              pad_sz = block_sz_ - align; 
            }

            char* buff;
            int ret = posix_memalign((void**)&buff, GetBlockSize(), data_size);

            if(ret) {
              fprintf(stderr, "Zone Cleaning : failed allocating alignment write buffer\n");
            }

            int f = GetReadFD();
            int f_direct = GetReadDirectFD();
            ssize_t r = 0;
            uint64_t r_off = zone_extent->start_;

            r = pread(f, buff, zone_extent->length_, r_off);
            
            if (r < 0) {
              r = pread(f_direct, buff, zone_extent->length_, r_off);
            }
            assert(r >= 0);
            
            if (pad_sz > 0) {
              memset((char*)buff + valid_size, 0x0, pad_sz); 
            }

            //allocate Zone and write contents.
            allocated_zone = AllocateZoneForCleaning();
            assert(allocated_zone);

            //Copy contents to new zone.
            {
                IOStatus s;
                uint32_t left = data_size;
                uint32_t wr_size, offset = 0;
                uint32_t new_extent_length = 0;
                std::vector<ZoneExtent *> new_zone_extents;

                while (left) { 
                    assert(allocated_zone);                   
                    if(left <= allocated_zone->capacity_){

                    //There'are enough room for write original extent
                        s = allocated_zone->Append((char*)buff + offset, left);
#ifdef EXPERIMENT
                        copied_data += (uint64_t)left;
#endif
                        allocated_zone->used_capacity_ += left;

                        ZoneExtent * new_extent = new ZoneExtent((allocated_zone->wp_ - left), /*Extent length*/left-pad_sz, allocated_zone);
                        ZoneExtentInfo * new_extent_info = new ZoneExtentInfo(new_extent, zone_file ,true, left-pad_sz, new_extent->start_, allocated_zone, zone_file->GetFilename(), zone_file->GetWriteLifeTimeHint(), zone_file->level_);
                        allocated_zone->PushExtentInfo(new_extent_info);
                        new_zone_extents.push_back(new_extent);
                        
                        allocated_zone->open_for_write_ = false;
                        open_io_zones_--;
                        
                        sst_zone_mtx_.lock();
                        if (zone_file->is_sst_) { 
                          std::vector<int> fz = sst_to_zone_[zone_file->fno_];
                          for (auto it = fz.begin(); it != fz.end(); it++) {
                             if (*it == victim_zone_id ) {
                               fz.erase(it);
                               break;
                             } 
                          }
                          sst_to_zone_[zone_file->fno_].push_back(allocated_zone->zone_id_);
                        }
                        sst_zone_mtx_.unlock();

                        new_extent_length +=left-pad_sz;
                        break; /*left = 0*/
                    } else {  
                        wr_size = allocated_zone->capacity_;
                        s = allocated_zone->Append((char*)buff + offset, wr_size);
                        assert(s.ok()); 
#ifdef EXPERIMENT
                        copied_data += (uint64_t)wr_size;
#endif
                        allocated_zone->used_capacity_ += wr_size;

                        left -= wr_size;
                        offset += wr_size;
                        assert(allocated_zone->capacity_ == 0); 

                        ZoneExtent * new_extent = new ZoneExtent((allocated_zone->wp_ - wr_size), /*Extent length*/wr_size, allocated_zone);

                        ZoneExtentInfo * new_extent_info = new ZoneExtentInfo(new_extent, zone_file ,true, wr_size,new_extent->start_, allocated_zone, zone_file->GetFilename(), zone_file->GetWriteLifeTimeHint(), zone_file->level_);
                        allocated_zone->PushExtentInfo(new_extent_info);    
                        
                        new_extent_length += wr_size;
                        new_zone_extents.push_back(new_extent);

                        sst_zone_mtx_.lock();
                        if (zone_file->is_sst_) {
                          std::vector<int> fz = sst_to_zone_[zone_file->fno_];
                          for (auto it = fz.begin(); it != fz.end(); it++) {
                             if (*it == victim_zone_id ) {
                               fz.erase(it);
                               break;
                             } 
                          }
                          sst_to_zone_[zone_file->fno_].push_back(allocated_zone->zone_id_);
                        }
                        sst_zone_mtx_.unlock();
                        //update and notify resource status
                        allocated_zone->open_for_write_ = false;
                        open_io_zones_--;
 
                        allocated_zone->Finish();
                        active_io_zones_--;
    
                        for (auto it = reserved_zones.begin(); it != reserved_zones.end(); ){
                          Zone * zz = (*it);
                          if (allocated_zone == zz){
                             io_zones.push_back(*it);
                             reserved_zones.erase(it);
                             break;
                          }
                          ++it;
                        }
                        //newly allocate new zone for write
                        allocated_zone = AllocateZoneForCleaning();
                        assert(allocated_zone);
                    }
                }//end of while.
           
                assert(new_extent_length == valid_size);
                assert(cur_victim->used_capacity_ >= zone_extent->length_); 
                cur_victim->used_capacity_ -= zone_extent->length_; 
                //update extent information of the file.
                //Replace origin extent information with newly made extent list.
                std::vector<ZoneExtent *> origin_extents_ = zone_file->GetExtentsList();
                std::vector<ZoneExtent *> replace_extents_;

                for (auto ze : origin_extents_) {
                  if (zone_extent == ze) {
                    for (auto new_ze : new_zone_extents) {
                      replace_extents_.push_back(new_ze);
                    }
                  } else {
                    replace_extents_.push_back(ze);
                  }
                }
                zone_file->UpdateExtents(replace_extents_);
                zone_file->ExtentWriteUnlock();
            }            
            free(buff);
        }
        assert(!cur_victim->open_for_write_);
        cur_victim->used_capacity_.store(0);
        cur_victim->Reset();
        active_io_zones_--;
        reseted++;
        for (auto it = io_zones.begin(); it != io_zones.end(); it++){
          if ((*it)->zone_id_ == cur_victim->zone_id_) {
            if (reserved_zones.size() < RESERVED_ZONE_FOR_CLEANING){
              io_zones.erase(it);
              reserved_zones.push_back(cur_victim);
            }
            break;
          }
        }
        gc_queue_.pop();
        if (reseted >= nr_reset) break;
    }
#ifdef EXPERIMENT
    fprintf(stdout, "Total Copied Data in ZC : %lu\n", copied_data);
#endif

    for ( auto it = reserved_zones.begin(); it !=reserved_zones.end(); ){
        if ( !((*it)->IsEmpty()) || ((*it)->IsUsed())) {
            io_zones.push_back(*it);
            reserved_zones.erase(it);
        } else {
            ++it;
        }
    }
    if (reserved_zones.size() < RESERVED_ZONE_FOR_CLEANING) {
      for ( auto it = io_zones.begin(); it !=io_zones.end(); ){
       if ( ((*it)->IsEmpty()) && !((*it)->open_for_write_)) {
            reserved_zones.push_back(*it);
            io_zones.erase(it);
        } else {
            ++it;
        }

       if(reserved_zones.size() == RESERVED_ZONE_FOR_CLEANING) 
         break;
      }
    }

    if (reserved_zones.size() > RESERVED_ZONE_FOR_CLEANING) {

      for ( auto it = reserved_zones.begin(); it !=reserved_zones.end(); ){
        if ( reserved_zones.size() != RESERVED_ZONE_FOR_CLEANING) {
            assert((*it)->IsEmpty() && !((*it)->open_for_write_));
            io_zones.push_back(*it);
            reserved_zones.erase(it);
        } else {
            ++it;
        }
      }
    }
    for (auto it = reserved_zones.begin(); it != reserved_zones.end(); it++){
        (*it)->used_capacity_.store(0);
    }
    zone_cleaning_mtx.unlock();
    return 1;
}//ZoneCleaning();
}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN) && defined(LIBZBD)
