// project headers
#include "terark_zip_table_reader.h"
#include "terark_zip_common.h"
// boost headers
#include <boost/scope_exit.hpp>
// rocksdb headers
#include <table/internal_iterator.h>
#include <table/sst_file_writer_collectors.h>
#include <table/meta_blocks.h>
#include <table/get_context.h>
// terark headers
#include <terark/util/crc.hpp>


namespace {
using namespace rocksdb;

// copy & modify from block_based_table_reader.cc
SequenceNumber GetGlobalSequenceNumber(const TableProperties& table_properties,
  Logger* info_log) {
  auto& props = table_properties.user_collected_properties;

  auto version_pos = props.find(ExternalSstFilePropertyNames::kVersion);
  auto seqno_pos = props.find(ExternalSstFilePropertyNames::kGlobalSeqno);

  if (version_pos == props.end()) {
    if (seqno_pos != props.end()) {
      // This is not an external sst file, global_seqno is not supported.
      assert(false);
      fprintf(stderr,
        "A non-external sst file have global seqno property with value %s\n",
        seqno_pos->second.c_str());
    }
    return kDisableGlobalSequenceNumber;
  }

  uint32_t version = DecodeFixed32(version_pos->second.c_str());
  if (version < 2) {
    if (seqno_pos != props.end() || version != 1) {
      // This is a v1 external sst file, global_seqno is not supported.
      assert(false);
      fprintf(stderr,
        "An external sst file with version %u have global seqno property "
        "with value %s\n",
        version, seqno_pos->second.c_str());
    }
    return kDisableGlobalSequenceNumber;
  }

  SequenceNumber global_seqno = DecodeFixed64(seqno_pos->second.c_str());

  if (global_seqno > kMaxSequenceNumber) {
    assert(false);
    fprintf(stderr,
      "An external sst file with version %u have global seqno property "
      "with value %llu, which is greater than kMaxSequenceNumber\n",
      version, (long long)global_seqno);
  }

  return global_seqno;
}

Block* DetachBlockContents(BlockContents &tombstoneBlock, SequenceNumber global_seqno)
{
  std::unique_ptr<char[]> tombstoneBuf(new char[tombstoneBlock.data.size()]);
  memcpy(tombstoneBuf.get(), tombstoneBlock.data.data(), tombstoneBlock.data.size());
#ifndef _MSC_VER
  uintptr_t ptr = (uintptr_t)tombstoneBlock.data.data();
  uintptr_t aligned_ptr = terark::align_up(ptr, 4096);
  if (aligned_ptr - ptr < tombstoneBlock.data.size()) {
    size_t sz = terark::align_down(
      tombstoneBlock.data.size() - (aligned_ptr - ptr), 4096);
    if (sz > 0) {
      madvise((void*)aligned_ptr, sz, MADV_DONTNEED);
    }
  }
#endif
  return new Block(
    BlockContents(std::move(tombstoneBuf), tombstoneBlock.data.size(), false, kNoCompression),
    global_seqno);
}

void SharedBlockCleanupFunction(void* arg1, void* arg2) {
  delete reinterpret_cast<shared_ptr<Block>*>(arg1);
}


static void MmapWarmUpBytes(const void* addr, size_t len) {
  auto base = (const byte_t*)(uintptr_t(addr) & uintptr_t(~4095));
  auto size = terark::align_up((size_t(addr) & 4095) + len, 4096);
#ifdef POSIX_MADV_WILLNEED
  posix_madvise((void*)addr, len, POSIX_MADV_WILLNEED);
#endif
  for (size_t i = 0; i < size; i += 4096) {
    volatile byte_t unused = ((const volatile byte_t*)base)[i];
    (void)unused;
  }
}
template<class T>
static void MmapWarmUp(const T* addr, size_t len) {
  MmapWarmUpBytes(addr, sizeof(T)*len);
}
static void MmapWarmUp(fstring mem) {
  MmapWarmUpBytes(mem.data(), mem.size());
}
template<class Vec>
static void MmapWarmUp(const Vec& uv) {
  MmapWarmUpBytes(uv.data(), uv.mem_size());
}


}


namespace rocksdb {

using terark::BadCrc32cException;
using terark::byte_swap;
using terark::BlobStore;

template<bool reverse>
class TerarkZipTableIterator : public InternalIterator, boost::noncopyable {
protected:
  const TableReaderOptions* table_reader_options_;
  const TerarkZipSegment* segment_;
  unique_ptr<TerarkIndex::Iterator> iter_;
  SequenceNumber          global_seqno_;
  ParsedInternalKey       pInterKey_;
  std::string             interKeyBuf_;
  valvec<byte_t>          interKeyBuf_xx_;
  valvec<byte_t>          valueBuf_;
  Slice                   userValue_;
  ZipValueType            zValtype_;
  size_t                  valnum_;
  size_t                  validx_;
  Status                  status_;
  PinnedIteratorsManager* pinned_iters_mgr_;
  valvec<valvec<byte_t>>  pinned_buffer_;

public:
  TerarkZipTableIterator(const TableReaderOptions& tro
    , const TerarkZipSegment *segment
    , SequenceNumber global_seqno)
    : table_reader_options_(&tro)
    , segment_(segment)
    , global_seqno_(global_seqno) {
    if (segment_ != nullptr) {
      iter_.reset(segment_->index_->NewIterator());
      iter_->SetInvalid();
    }
    pinned_iters_mgr_ = NULL;
    TryPinBuffer(interKeyBuf_xx_);
    validx_ = 0;
    valnum_ = 0;
    pInterKey_.user_key = Slice();
    pInterKey_.sequence = uint64_t(-1);
    pInterKey_.type = kMaxValue;
  }

  void SetPinnedItersMgr(PinnedIteratorsManager* pinned_iters_mgr) {
    if (pinned_iters_mgr_ && pinned_iters_mgr_ != pinned_iters_mgr) {
      pinned_buffer_.clear();
    }
    pinned_iters_mgr_ = pinned_iters_mgr;
  }

  bool Valid() const override {
    return iter_->Valid();
  }

  void SeekToFirst() override {
    if (UnzipIterRecord(IndexIterSeekToFirst())) {
      DecodeCurrKeyValue();
    }
  }

  void SeekToLast() override {
    if (UnzipIterRecord(IndexIterSeekToLast())) {
      validx_ = valnum_ - 1;
      DecodeCurrKeyValue();
    }
  }

  void SeekForPrev(const Slice& target) override {
    SeekForPrevImpl(target, &table_reader_options_->internal_comparator);
  }

  void Seek(const Slice& target) override {
    ParsedInternalKey pikey;
    if (!ParseInternalKey(target, &pikey)) {
      status_ = Status::InvalidArgument("TerarkZipTableIterator::Seek()",
        "param target.size() < 8");
      SetIterInvalid();
      return;
    }
    SeekInternal(pikey);
  }

  void SeekInternal(const ParsedInternalKey& pikey) {
    TryPinBuffer(interKeyBuf_xx_);
    // Damn MySQL-rocksdb may use "rev:" comparator
    size_t cplen = fstringOf(pikey.user_key).commonPrefixLen(segment_->commonPrefix_);
    if (segment_->commonPrefix_.size() != cplen) {
      if (pikey.user_key.size() == cplen) {
        assert(pikey.user_key.size() < segment_->commonPrefix_.size());
        if (reverse) {
          SeekToLast();
          this->Next(); // move  to EOF
          assert(!this->Valid());
        }
        else {
          SeekToFirst();
        }
      }
      else {
        assert(pikey.user_key.size() > cplen);
        assert(pikey.user_key[cplen] != segment_->commonPrefix_[cplen]);
        if ((byte_t(pikey.user_key[cplen]) < segment_->commonPrefix_[cplen]) ^ reverse) {
          SeekToFirst();
        }
        else {
          SeekToLast();
          this->Next(); // move  to EOF
          assert(!this->Valid());
        }
      }
    }
    else {
      bool ok;
      int cmp; // compare(iterKey, searchKey)
      if (reverse) {
        ok = iter_->Seek(fstringOf(pikey.user_key).substr(cplen));
        if (!ok) {
          // searchKey is reverse_bytewise less than all keys in database
          iter_->SeekToLast();
          ok = iter_->Valid();
          cmp = -1;
        }
        else if ((cmp = SliceOf(iter_->key()).compare(SubStr(pikey.user_key, cplen))) != 0) {
          iter_->Prev();
          ok = iter_->Valid();
        }
      }
      else {
        ok = iter_->Seek(fstringOf(pikey.user_key).substr(cplen));
        if (ok) {
          cmp = SliceOf(iter_->key()).compare(SubStr(pikey.user_key, cplen));
        }
      }
      if (UnzipIterRecord(ok)) {
        if (0 == cmp) {
          validx_ = size_t(-1);
          do {
            validx_++;
            DecodeCurrKeyValue();
            if (pInterKey_.sequence <= pikey.sequence) {
              return; // done
            }
          } while (validx_ + 1 < valnum_);
          // no visible version/sequence for target, use Next();
          // if using Next(), version check is not needed
          Next();
        }
        else {
          DecodeCurrKeyValue();
        }
      }
    }
  }

  void Next() override {
    assert(iter_->Valid());
    validx_++;
    if (validx_ < valnum_) {
      DecodeCurrKeyValue();
    }
    else {
      if (UnzipIterRecord(IndexIterNext())) {
        DecodeCurrKeyValue();
      }
    }
  }

  void Prev() override {
    assert(iter_->Valid());
    if (validx_ > 0) {
      validx_--;
      DecodeCurrKeyValue();
    }
    else {
      if (UnzipIterRecord(IndexIterPrev())) {
        validx_ = valnum_ - 1;
        DecodeCurrKeyValue();
      }
    }
  }

  Slice key() const override {
    assert(iter_->Valid());
    return SliceOf(interKeyBuf_xx_);
  }

  Slice value() const override {
    assert(iter_->Valid());
    return userValue_;
  }

  Status status() const override {
    return status_;
  }

  bool IsKeyPinned() const {
    return pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled();
  }
  bool IsValuePinned() const {
    return pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled();
  }

protected:
  virtual void SetIterInvalid() {
    TryPinBuffer(interKeyBuf_xx_);
    iter_->SetInvalid();
    validx_ = 0;
    valnum_ = 0;
    pInterKey_.user_key = Slice();
    pInterKey_.sequence = uint64_t(-1);
    pInterKey_.type = kMaxValue;
  }
  virtual bool IndexIterSeekToFirst() {
    TryPinBuffer(interKeyBuf_xx_);
    if (reverse)
      return iter_->SeekToLast();
    else
      return iter_->SeekToFirst();
  }
  virtual bool IndexIterSeekToLast() {
    TryPinBuffer(interKeyBuf_xx_);
    if (reverse)
      return iter_->SeekToFirst();
    else
      return iter_->SeekToLast();
  }
  virtual bool IndexIterPrev() {
    TryPinBuffer(interKeyBuf_xx_);
    if (reverse)
      return iter_->Next();
    else
      return iter_->Prev();
  }
  virtual bool IndexIterNext() {
    TryPinBuffer(interKeyBuf_xx_);
    if (reverse)
      return iter_->Prev();
    else
      return iter_->Next();
  }
  virtual void DecodeCurrKeyValue() {
    DecodeCurrKeyValueInternal();
    interKeyBuf_.assign(segment_->commonPrefix_.data(), segment_->commonPrefix_.size());
    AppendInternalKey(&interKeyBuf_, pInterKey_);
    interKeyBuf_xx_.assign((byte_t*)interKeyBuf_.data(), interKeyBuf_.size());
  }
  void TryPinBuffer(valvec<byte_t>& buf) {
    if (pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled()) {
      pinned_buffer_.push_back();
      pinned_buffer_.back().swap(buf);
    }
  }
  bool UnzipIterRecord(bool hasRecord) {
    if (hasRecord) {
      size_t recId = iter_->id();
      zValtype_ = segment_->type_.size()
        ? ZipValueType(segment_->type_[recId])
        : ZipValueType::kZeroSeq;
      try {
        TryPinBuffer(valueBuf_);
        if (ZipValueType::kMulti == zValtype_) {
          valueBuf_.resize_no_init(sizeof(uint32_t)); // for offsets[valnum_]
        }
        else {
          valueBuf_.erase_all();
        }
        segment_->store_->get_record_append(recId, &valueBuf_);
      }
      catch (const BadCrc32cException& ex) { // crc checksum error
        SetIterInvalid();
        status_ = Status::Corruption(
          "TerarkZipTableIterator::UnzipIterRecord()", ex.what());
        return false;
      }
      if (ZipValueType::kMulti == zValtype_) {
        ZipValueMultiValue::decode(valueBuf_, &valnum_);
      }
      else {
        valnum_ = 1;
      }
      validx_ = 0;
      pInterKey_.user_key = SliceOf(iter_->key());
      return true;
    }
    else {
      SetIterInvalid();
      return false;
    }
  }
  void DecodeCurrKeyValueInternal() {
    assert(status_.ok());
    assert(iter_->id() < segment_->index_->NumKeys());
    switch (zValtype_) {
    default:
      status_ = Status::Aborted("TerarkZipTableIterator::DecodeCurrKeyValue()",
        "Bad ZipValueType");
      abort(); // must not goes here, if it does, it should be a bug!!
      break;
    case ZipValueType::kZeroSeq:
      assert(0 == validx_);
      assert(1 == valnum_);
      pInterKey_.sequence = global_seqno_;
      pInterKey_.type = kTypeValue;
      userValue_ = SliceOf(valueBuf_);
      break;
    case ZipValueType::kValue: // should be a kTypeValue, the normal case
      assert(0 == validx_);
      assert(1 == valnum_);
      // little endian uint64_t
      pInterKey_.sequence = *(uint64_t*)valueBuf_.data() & kMaxSequenceNumber;
      pInterKey_.type = kTypeValue;
      userValue_ = SliceOf(fstring(valueBuf_).substr(7));
      break;
    case ZipValueType::kDelete:
      assert(0 == validx_);
      assert(1 == valnum_);
      // little endian uint64_t
      pInterKey_.sequence = *(uint64_t*)valueBuf_.data() & kMaxSequenceNumber;
      pInterKey_.type = kTypeDeletion;
      userValue_ = Slice();
      break;
    case ZipValueType::kMulti: { // more than one value
      auto zmValue = (const ZipValueMultiValue*)(valueBuf_.data());
      assert(0 != valnum_);
      assert(validx_ < valnum_);
      Slice d = zmValue->getValueData(validx_, valnum_);
      auto snt = unaligned_load<SequenceNumber>(d.data());
      UnPackSequenceAndType(snt, &pInterKey_.sequence, &pInterKey_.type);
      d.remove_prefix(sizeof(SequenceNumber));
      userValue_ = d;
      break; }
    }
  }
};


#if defined(TERARK_SUPPORT_UINT64_COMPARATOR) && BOOST_ENDIAN_LITTLE_BYTE
class TerarkZipTableUint64Iterator : public TerarkZipTableIterator<false> {
public:
  TerarkZipTableUint64Iterator(const TableReaderOptions& tro
    , const TerarkZipSegment *segment
    , SequenceNumber global_seqno)
    : TerarkZipTableIterator<false>(tro, segment, global_seqno) {
  }
  void Seek(const Slice& target) override {
    ParsedInternalKey pikey;
    if (!ParseInternalKey(target, &pikey)) {
      status_ = Status::InvalidArgument("TerarkZipTableIterator::Seek()",
        "param target.size() < 8");
      SetIterInvalid();
      return;
    }
    uint64_t u64_target;
    assert(pikey.user_key.size() == 8);
    u64_target = byte_swap(*reinterpret_cast<const uint64_t*>(pikey.user_key.data()));
    pikey.user_key = Slice(reinterpret_cast<const char*>(&u64_target), 8);
    SeekInternal(pikey);
  }
  void DecodeCurrKeyValue() override {
    DecodeCurrKeyValueInternal();
    interKeyBuf_.assign(segment_->commonPrefix_.data(), segment_->commonPrefix_.size());
    AppendInternalKey(&interKeyBuf_, pInterKey_);
    assert(interKeyBuf_.size() == 16);
    uint64_t *ukey = reinterpret_cast<uint64_t*>(&interKeyBuf_[0]);
    *ukey = byte_swap(*ukey);
    interKeyBuf_xx_.assign((byte_t*)interKeyBuf_.data(), interKeyBuf_.size());
  }
};
#endif

#if defined(TerocksPrivateCode)

template<bool reverse>
class TerarkZipTableMultiIterator : public TerarkZipTableIterator<reverse> {
public:
  TerarkZipTableMultiIterator(const TableReaderOptions& tro
    , const TerarkZipTableMultiReader::SegmentIndex& segmentIndex
    , SequenceNumber global_seqno)
    : TerarkZipTableIterator<reverse>(tro, nullptr, global_seqno)
    , segmentIndex_(&segmentIndex) {
  }
protected:
  const TerarkZipTableMultiReader::SegmentIndex *segmentIndex_;

public:
  bool Valid() const override {
    return iter_ && iter_->Valid();
  }

  void Seek(const Slice& target) override {
    ParsedInternalKey pikey;
    if (!ParseInternalKey(target, &pikey)) {
      status_ = Status::InvalidArgument("TerarkZipTableIterator::Seek()",
        "param target.size() < 8");
      SetIterInvalid();
      return;
    }
    auto segment = segmentIndex_->GetSegment(fstringOf(pikey.user_key));
    if (segment == nullptr) {
      SetIterInvalid();
      return;
    }
    pikey.user_key.remove_prefix(std::min(segment->prefix_.size(), pikey.user_key.size()));
    if (segment != segment_) {
      segment_ = segment;
      iter_.reset(segment_->index_->NewIterator());
    }
    SeekInternal(pikey);
    if (!Valid()) {
      if (reverse) {
        if (segment->segmentIndex_ != 0) {
          segment_ = segmentIndex_->GetSegment(segment->segmentIndex_ - 1);
          iter_.reset(segment_->index_->NewIterator());
          if (UnzipIterRecord(iter_->SeekToLast())) {
            validx_ = valnum_ - 1;
            DecodeCurrKeyValue();
          }
        }
      }
      else {
        if (segment->segmentIndex_ != segmentIndex_->GetSegmentCount() - 1) {
          segment_ = segmentIndex_->GetSegment(segment->segmentIndex_ + 1);
          iter_.reset(segment_->index_->NewIterator());
          if (UnzipIterRecord(iter_->SeekToFirst())) {
            DecodeCurrKeyValue();
          }
        }
      }
    }
  }

protected:
  void SetIterInvalid() override {
    TryPinBuffer(interKeyBuf_xx_);
    segment_ = nullptr;
    iter_.reset();
    validx_ = 0;
    valnum_ = 0;
    pInterKey_.user_key = Slice();
    pInterKey_.sequence = uint64_t(-1);
    pInterKey_.type = kMaxValue;
  }
  bool IndexIterSeekToFirst() override {
    TryPinBuffer(interKeyBuf_xx_);
    if (reverse) {
      segment_ = segmentIndex_->GetSegment(segmentIndex_->GetSegmentCount() - 1);
      iter_.reset(segment_->index_->NewIterator());
      return iter_->SeekToLast();
    }
    else {
      segment_ = segmentIndex_->GetSegment(0);
      iter_.reset(segment_->index_->NewIterator());
      return iter_->SeekToFirst();
    }
  }
  bool IndexIterSeekToLast() override {
    TryPinBuffer(interKeyBuf_xx_);
    if (reverse) {
      segment_ = segmentIndex_->GetSegment(0);
      iter_.reset(segment_->index_->NewIterator());
      return iter_->SeekToFirst();
    }
    else {
      segment_ = segmentIndex_->GetSegment(segmentIndex_->GetSegmentCount() - 1);
      iter_.reset(segment_->index_->NewIterator());
      return iter_->SeekToLast();
    }
  }
  bool IndexIterPrev() override {
    TryPinBuffer(interKeyBuf_xx_);
    if (reverse) {
      if (iter_->Next()) {
        return true;
      }
      if (segment_->segmentIndex_ == segmentIndex_->GetSegmentCount() - 1) {
        return false;
      }
      segment_ = segmentIndex_->GetSegment(segment_->segmentIndex_ + 1);
      iter_.reset(segment_->index_->NewIterator());
      return iter_->SeekToFirst();
    }
    else {
      if (iter_->Prev()) {
        return true;
      }
      if (segment_->segmentIndex_ == 0) {
        return false;
      }
      segment_ = segmentIndex_->GetSegment(segment_->segmentIndex_ - 1);
      iter_.reset(segment_->index_->NewIterator());
      return iter_->SeekToLast();
    }
  }
  bool IndexIterNext() override {
    TryPinBuffer(interKeyBuf_xx_);
    if (reverse) {
      if (iter_->Prev()) {
        return true;
      }
      if (segment_->segmentIndex_ == 0) {
        return false;
      }
      segment_ = segmentIndex_->GetSegment(segment_->segmentIndex_ - 1);
      iter_.reset(segment_->index_->NewIterator());
      return iter_->SeekToLast();
    }
    else {
      if (iter_->Next()) {
        return true;
      }
      if (segment_->segmentIndex_ == segmentIndex_->GetSegmentCount() - 1) {
        return false;
      }
      segment_ = segmentIndex_->GetSegment(segment_->segmentIndex_ + 1);
      iter_.reset(segment_->index_->NewIterator());
      return iter_->SeekToFirst();
    }
  }
  void DecodeCurrKeyValue() override {
    DecodeCurrKeyValueInternal();
    interKeyBuf_.assign(segment_->prefix_.data(), segment_->prefix_.size());
    interKeyBuf_.append(segment_->commonPrefix_.data(), segment_->commonPrefix_.size());
    AppendInternalKey(&interKeyBuf_, pInterKey_);
    interKeyBuf_xx_.assign((byte_t*)interKeyBuf_.data(), interKeyBuf_.size());
  }
};

#endif // TerocksPrivateCode

Status rocksdb::TerarkZipTableTombstone::
LoadTombstone(RandomAccessFileReader * file, uint64_t file_size) {
  BlockContents tombstoneBlock;
  Status s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, 
    GetTableReaderOptions().ioptions,  kRangeDelBlock, &tombstoneBlock);
  if (s.ok()) {
    tombstone_.reset(DetachBlockContents(tombstoneBlock, GetSequenceNumber()));
  }
  return s;
}

InternalIterator *TerarkZipTableTombstone::
NewRangeTombstoneIterator(const ReadOptions & read_options) {
  if (tombstone_) {
    auto iter = tombstone_->NewIterator(
      &GetTableReaderOptions().internal_comparator,
      nullptr, true,
      GetTableReaderOptions().ioptions.statistics);
    iter->RegisterCleanup(SharedBlockCleanupFunction,
      new shared_ptr<Block>(tombstone_), nullptr);
    return iter;
  }
  return nullptr;
}


Status TerarkZipSegment::Get(SequenceNumber global_seqno, const ReadOptions& ro, const Slice& ikey,
  GetContext* get_context, int flag) const {
  (void)flag;
  MY_THREAD_LOCAL(valvec<byte_t>, g_tbuf);
  ParsedInternalKey pikey;
  if (!ParseInternalKey(ikey, &pikey)) {
    return Status::InvalidArgument("TerarkZipTableReader::Get()",
      "bad internal key causing ParseInternalKey() failed");
  }
  Slice user_key = pikey.user_key;
#if defined(TERARK_SUPPORT_UINT64_COMPARATOR) && BOOST_ENDIAN_LITTLE_BYTE
  uint64_t u64_target;
  if (flag & FlagUint64Comparator) {
    assert(pikey.user_key.size() == 8);
    u64_target = byte_swap(*reinterpret_cast<const uint64_t*>(pikey.user_key.data()));
    user_key = Slice(reinterpret_cast<const char*>(&u64_target), 8);
  }
#endif
  size_t cplen = user_key.difference_offset(commonPrefix_);
  if (commonPrefix_.size() != cplen) {
    return Status::OK();
  }
  assert(user_key.size() > prefix_.size());
  size_t recId = index_->Find(fstringOf(user_key).substr(cplen + prefix_.size()));
  if (size_t(-1) == recId) {
    return Status::OK();
  }
  auto zvType = type_.size()
    ? ZipValueType(type_[recId])
    : ZipValueType::kZeroSeq;
  if (ZipValueType::kMulti == zvType) {
    g_tbuf.resize_no_init(sizeof(uint32_t));
  }
  else {
    g_tbuf.erase_all();
  }
  try {
    store_->get_record_append(recId, &g_tbuf);
  }
  catch (const terark::BadChecksumException& ex) {
    return Status::Corruption("TerarkZipTableReader::Get()", ex.what());
  }
  switch (zvType) {
  default:
    return Status::Aborted("TerarkZipTableReader::Get()", "Bad ZipValueType");
  case ZipValueType::kZeroSeq:
    get_context->SaveValue(ParsedInternalKey(pikey.user_key, global_seqno, kTypeValue),
      Slice((char*)g_tbuf.data(), g_tbuf.size()));
    break;
  case ZipValueType::kValue: { // should be a kTypeValue, the normal case
                               // little endian uint64_t
    uint64_t seq = *(uint64_t*)g_tbuf.data() & kMaxSequenceNumber;
    if (seq <= pikey.sequence) {
      get_context->SaveValue(ParsedInternalKey(pikey.user_key, seq, kTypeValue),
        SliceOf(fstring(g_tbuf).substr(7)));
    }
    break; }
  case ZipValueType::kDelete: {
    // little endian uint64_t
    uint64_t seq = *(uint64_t*)g_tbuf.data() & kMaxSequenceNumber;
    if (seq <= pikey.sequence) {
      get_context->SaveValue(ParsedInternalKey(pikey.user_key, seq, kTypeDeletion),
        Slice());
    }
    break; }
  case ZipValueType::kMulti: { // more than one value
    size_t num = 0;
    auto mVal = ZipValueMultiValue::decode(g_tbuf, &num);
    for (size_t i = 0; i < num; ++i) {
      Slice val = mVal->getValueData(i, num);
      SequenceNumber sn;
      ValueType valtype;
      {
        auto snt = unaligned_load<SequenceNumber>(val.data());
        UnPackSequenceAndType(snt, &sn, &valtype);
      }
      if (sn <= pikey.sequence) {
        val.remove_prefix(sizeof(SequenceNumber));
        // only kTypeMerge will return true
        bool hasMoreValue = get_context->SaveValue(
          ParsedInternalKey(pikey.user_key, sn, valtype), val);
        if (!hasMoreValue) {
          break;
        }
      }
    }
    break; }
  }
  if (g_tbuf.capacity() > 512 * 1024) {
    g_tbuf.clear(); // free large thread local memory
  }
  return Status::OK();
}

TerarkZipSegment::~TerarkZipSegment() {
  type_.risk_release_ownership();
}

Status
TerarkEmptyTableReader::Open(RandomAccessFileReader* file, uint64_t file_size) {
  file_.reset(file); // take ownership
  const auto& ioptions = table_reader_options_.ioptions;
  TableProperties* props = nullptr;
  Status s = ReadTableProperties(file, file_size,
    kTerarkZipTableMagicNumber, ioptions, &props);
  if (!s.ok()) {
    return s;
  }
  assert(nullptr != props);
  unique_ptr<TableProperties> uniqueProps(props);
  Slice file_data;
  if (table_reader_options_.env_options.use_mmap_reads) {
    s = file->Read(0, file_size, &file_data, nullptr);
    if (!s.ok())
      return s;
  }
  else {
    return Status::InvalidArgument("TerarkZipTableReader::Open()",
      "EnvOptions::use_mmap_reads must be true");
  }
  file_data_ = file_data;
  table_properties_.reset(uniqueProps.release());
  global_seqno_ = GetGlobalSequenceNumber(*table_properties_, ioptions.info_log);
#if defined(TerocksPrivateCode)
  auto table_factory = dynamic_cast<TerarkZipTableFactory*>(ioptions.table_factory);
  assert(table_factory);
  auto& license = table_factory->GetLicense();
  BlockContents licenseBlock;
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableExtendedBlock, &licenseBlock);
  if (s.ok()) {
    auto res = license.merge(licenseBlock.data.data(), licenseBlock.data.size());
    assert(res == LicenseInfo::Result::OK);
    (void)res; // shut up !
    if (!license.check()) {
      license.print_error(nullptr, false, ioptions.info_log);
      return Status::Corruption("License expired", "contact@terark.com");
    }
  }
#endif // TerocksPrivateCode
  s = LoadTombstone(file, file_size);
  if (global_seqno_ == kDisableGlobalSequenceNumber) {
    global_seqno_ = 0;
  }
  INFO(ioptions.info_log
    , "TerarkZipTableReader::Open(): fsize = %zd, entries = %zd keys = 0 indexSize = 0 valueSize = 0, warm up time =      0.000'sec, build cache time =      0.000'sec\n"
    , size_t(file_size), size_t(table_properties_->num_entries)
  );
  return Status::OK();
}


Status
TerarkZipTableReader::Open(RandomAccessFileReader* file, uint64_t file_size) {
  file_.reset(file); // take ownership
  const auto& ioptions = table_reader_options_.ioptions;
  TableProperties* props = nullptr;
  Status s = ReadTableProperties(file, file_size,
    kTerarkZipTableMagicNumber, ioptions, &props);
  if (!s.ok()) {
    return s;
  }
  assert(nullptr != props);
  unique_ptr<TableProperties> uniqueProps(props);
  Slice file_data;
  if (table_reader_options_.env_options.use_mmap_reads) {
    s = file->Read(0, file_size, &file_data, nullptr);
    if (!s.ok())
      return s;
  }
  else {
    return Status::InvalidArgument("TerarkZipTableReader::Open()",
      "EnvOptions::use_mmap_reads must be true");
  }
  file_data_ = file_data;
  table_properties_.reset(uniqueProps.release());
  global_seqno_ = GetGlobalSequenceNumber(*table_properties_, ioptions.info_log);
  isReverseBytewiseOrder_ =
    fstring(ioptions.user_comparator->Name()).startsWith("rev:");
#if defined(TERARK_SUPPORT_UINT64_COMPARATOR) && BOOST_ENDIAN_LITTLE_BYTE
  isUint64Comparator_ =
    fstring(ioptions.user_comparator->Name()) == "rocksdb.Uint64Comparator";
#endif
  BlockContents valueDictBlock, indexBlock, zValueTypeBlock, commonPrefixBlock;
#if defined(TerocksPrivateCode)
  BlockContents licenseBlock;
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableExtendedBlock, &licenseBlock);
  if (s.ok()) {
    auto table_factory = dynamic_cast<TerarkZipTableFactory*>(ioptions.table_factory);
    assert(table_factory);
    auto& license = table_factory->GetLicense();
    auto res = license.merge(licenseBlock.data.data(), licenseBlock.data.size());
    assert(res == LicenseInfo::Result::OK);
    (void)res; // shut up !
    if (!license.check()) {
      license.print_error(nullptr, false, ioptions.info_log);
      return Status::Corruption("License expired", "contact@terark.com");
    }
  }
#endif // TerocksPrivateCode
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableValueDictBlock, &valueDictBlock);
#if defined(TerocksPrivateCode)
  // PlainBlobStore & MixedLenBlobStore no dict
#endif // TerocksPrivateCode
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableIndexBlock, &indexBlock);
  if (!s.ok()) {
    return s;
  }
  s = LoadTombstone(file, file_size);
  if (global_seqno_ == kDisableGlobalSequenceNumber) {
    global_seqno_ = 0;
  }
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableCommonPrefixBlock, &commonPrefixBlock);
  if (s.ok()) {
    segment_.commonPrefix_.assign(commonPrefixBlock.data.data(),
      commonPrefixBlock.data.size());
  }
  else {
    // some error, usually is
    // Status::Corruption("Cannot find the meta block", meta_block_name)
    WARN(ioptions.info_log
      , "Read %s block failed, treat as old SST version, error: %s\n"
      , kTerarkZipTableCommonPrefixBlock.c_str()
      , s.ToString().c_str());
  }
  try {
    segment_.store_.reset(terark::BlobStore::load_from_user_memory(
      fstring(file_data.data(), props->data_size),
      fstringOf(valueDictBlock.data)
    ));
  }
  catch (const BadCrc32cException& ex) {
    return Status::Corruption("TerarkZipTableReader::Open()", ex.what());
  }
  s = LoadIndex(indexBlock.data);
  if (!s.ok()) {
    return s;
  }
  size_t recNum = segment_.index_->NumKeys();
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableValueTypeBlock, &zValueTypeBlock);
  if (s.ok()) {
    segment_.type_.risk_set_data((byte_t*)zValueTypeBlock.data.data(), recNum);
  }
  long long t0 = g_pf.now();
  if (tzto_.warmUpIndexOnOpen) {
    MmapWarmUp(fstringOf(indexBlock.data));
    if (!tzto_.warmUpValueOnOpen) {
      MmapWarmUp(segment_.store_->get_dict());
      for (fstring block : segment_.store_->get_index_blocks()) {
        MmapWarmUp(block);
      }
    }
  }
  if (tzto_.warmUpValueOnOpen) {
    MmapWarmUp(segment_.store_->get_mmap());
  }
  long long t1 = g_pf.now();
  segment_.index_->BuildCache(tzto_.indexCacheRatio);
  long long t2 = g_pf.now();
  INFO(ioptions.info_log
    , "TerarkZipTableReader::Open(): fsize = %zd, entries = %zd keys = %zd indexSize = %zd valueSize=%zd, warm up time = %6.3f'sec, build cache time = %6.3f'sec\n"
    , size_t(file_size), size_t(table_properties_->num_entries)
    , segment_.index_->NumKeys()
    , size_t(table_properties_->index_size)
    , size_t(table_properties_->data_size)
    , g_pf.sf(t0, t1)
    , g_pf.sf(t1, t2)
  );
  return Status::OK();
}



Status TerarkZipTableReader::LoadIndex(Slice mem) {
  auto func = "TerarkZipTableReader::LoadIndex()";
  try {
    segment_.index_ = TerarkIndex::LoadMemory(fstringOf(mem));
  }
  catch (const BadCrc32cException& ex) {
    return Status::Corruption(func, ex.what());
  }
  catch (const std::exception& ex) {
    return Status::InvalidArgument(func, ex.what());
  }
  return Status::OK();
}

InternalIterator*
TerarkZipTableReader::
NewIterator(const ReadOptions& ro, Arena* arena, bool skip_filters) {
  (void)skip_filters; // unused
#if defined(TERARK_SUPPORT_UINT64_COMPARATOR) && BOOST_ENDIAN_LITTLE_BYTE
  if (isUint64Comparator_) {
    if (arena) {
      return new(arena->AllocateAligned(sizeof(TerarkZipTableUint64Iterator)))
        TerarkZipTableUint64Iterator(table_reader_options_, &segment_, global_seqno_);
    }
    else {
      return new TerarkZipTableUint64Iterator(table_reader_options_, &segment_, global_seqno_);
    }
  }
#endif
  if (isReverseBytewiseOrder_) {
    if (arena) {
      return new(arena->AllocateAligned(sizeof(TerarkZipTableIterator<true>)))
        TerarkZipTableIterator<true>(table_reader_options_, &segment_, global_seqno_);
    }
    else {
      return new TerarkZipTableIterator<true>(table_reader_options_, &segment_, global_seqno_);
    }
  }
  else {
    if (arena) {
      return new(arena->AllocateAligned(sizeof(TerarkZipTableIterator<false>)))
        TerarkZipTableIterator<false>(table_reader_options_, &segment_, global_seqno_);
    }
    else {
      return new TerarkZipTableIterator<false>(table_reader_options_, &segment_, global_seqno_);
    }
  }
}


Status
TerarkZipTableReader::Get(const ReadOptions& ro, const Slice& ikey,
  GetContext* get_context, bool skip_filters) {
  int flag = skip_filters ? TerarkZipSegment::FlagSkipFilter : TerarkZipSegment::FlagNone;
#if defined(TERARK_SUPPORT_UINT64_COMPARATOR) && BOOST_ENDIAN_LITTLE_BYTE
  if (isUint64Comparator_) {
    flag |= TerarkZipSegment::FlagUint64Comparator;
  }
#endif
  return segment_.Get(global_seqno_, ro, ikey, get_context, flag);
}

TerarkZipTableReader::~TerarkZipTableReader() {
}

TerarkZipTableReader::TerarkZipTableReader(const TableReaderOptions& tro,
  const TerarkZipTableOptions& tzto)
  : table_reader_options_(tro)
  , global_seqno_(kDisableGlobalSequenceNumber)
  , tzto_(tzto)
{
  isReverseBytewiseOrder_ = false;
}

#if defined(TerocksPrivateCode)

fstring TerarkZipTableMultiReader::SegmentIndex::PartIndexOperator::operator[](size_t i) const {
  return fstring(p->prefixSet_.data() + i * p->alignedPrefixLen_, p->prefixLen_);
};

const TerarkZipSegment* TerarkZipTableMultiReader::SegmentIndex::GetSegmentU64Sequential(fstring key) const {
  byte_t targetBuffer[8] = {};
  memcpy(targetBuffer + (8 - prefixLen_), key.data(), std::min<size_t>(prefixLen_, key.size()));
  uint64_t targetValue = ReadUint64Aligned(targetBuffer, targetBuffer + 8);
  auto ptr = (const uint64_t*)prefixSet_.data();
  size_t count = partCount_;
  for (size_t i = 0; i < count; ++i) {
    if (ptr[i] >= targetValue) {
      return &segments_[i];
    }
  }
  return nullptr;
}

const TerarkZipSegment* TerarkZipTableMultiReader::SegmentIndex::GetSegmentU64Binary(fstring key) const {
  byte_t targetBuffer[8] = {};
  memcpy(targetBuffer + (8 - prefixLen_), key.data(), std::min<size_t>(prefixLen_, key.size()));
  uint64_t targetValue = ReadUint64Aligned(targetBuffer, targetBuffer + 8);
  auto ptr = (const uint64_t*)prefixSet_.data();
  auto index = terark::lower_bound_n(ptr, 0, partCount_, targetValue);
  if (index == partCount_) {
    return nullptr;
  }
  return &segments_[index];
}

const TerarkZipSegment* TerarkZipTableMultiReader::SegmentIndex::GetSegmentU64BinaryReverse(fstring key) const {
  byte_t targetBuffer[8] = {};
  memcpy(targetBuffer + (8 - prefixLen_), key.data(), std::min<size_t>(prefixLen_, key.size()));
  uint64_t targetValue = ReadUint64Aligned(targetBuffer, targetBuffer + 8);
  auto ptr = (const uint64_t*)prefixSet_.data();
  auto index = terark::upper_bound_n(ptr, 0, partCount_, targetValue);
  if (index == 0) {
    return nullptr;
  }
  return &segments_[index - 1];
}

const TerarkZipSegment* TerarkZipTableMultiReader::SegmentIndex::GetSegmentBytewise(fstring key) const {
  if (key.size() > prefixLen_) {
    key = fstring(key.data(), prefixLen_);
  }
  PartIndexOperator ptr = {this};
  auto index = terark::lower_bound_n(ptr, 0, partCount_, key);
  if (index == partCount_) {
    return nullptr;
  }
  return &segments_[index];
}

const TerarkZipSegment* TerarkZipTableMultiReader::SegmentIndex::GetSegmentBytewiseReverse(fstring key) const {
  if (key.size() > prefixLen_) {
    key = fstring(key.data(), prefixLen_);
  }
  PartIndexOperator ptr = {this};
  auto index = terark::upper_bound_n(ptr, 0, partCount_, key);
  if (index == 0) {
    return nullptr;
  }
  return &segments_[index - 1];
}

Status TerarkZipTableMultiReader::SegmentIndex::Init(
  fstring offsetMemory,
  fstring indexMempry,
  fstring storeMemory,
  fstring dictMemory,
  fstring typeMemory,
  fstring commonPrefixMemory,
  bool reverse) {
  TerarkZipMultiOffsetInfo offset;
  if (!offset.risk_set_memory(offsetMemory.data(), offsetMemory.size())) {
    return Status::Corruption("bad offset block");
  }
  BOOST_SCOPE_EXIT(&offset) {
    offset.risk_release_ownership();
  }BOOST_SCOPE_EXIT_END;
  segments_.reserve(offset.partCount_);

  partCount_ = offset.partCount_;
  prefixLen_ = offset.prefixLen_;
  alignedPrefixLen_ = terark::align_up(prefixLen_, 8);
  prefixSet_.resize(alignedPrefixLen_ * partCount_);

  if (prefixLen_ <= 8) {
    for (size_t i = 0; i < partCount_; ++i) {
      auto u64p = (uint64_t*)(prefixSet_.data() + i * alignedPrefixLen_);
      auto src = (const byte_t *)offset.prefixSet_.data() + i * prefixLen_;
      *u64p = ReadUint64(src, src + prefixLen_);
    }
    get_segment_ptr = reverse ? &SegmentIndex::GetSegmentU64BinaryReverse :
      partCount_ < 32 ? &SegmentIndex::GetSegmentU64Sequential : &SegmentIndex::GetSegmentU64Binary;
  }
  else {
    for (size_t i = 0; i < partCount_; ++i) {
      memcpy(prefixSet_.data() + i * alignedPrefixLen_,
        offset.prefixSet_.data() + i * prefixLen_, prefixLen_);
    }
    get_segment_ptr = reverse ? &SegmentIndex::GetSegmentBytewiseReverse : &SegmentIndex::GetSegmentBytewise;
  }

  TerarkZipMultiOffsetInfo::KeyValueOffset last = {0, 0, 0, 0};
  try {
    for (size_t i = 0; i < partCount_; ++i) {
      segments_.push_back();
      auto& part = segments_.back();
      auto& curr = offset.offset_[i];
      part.segmentIndex_ = i;
      part.prefix_.assign(offset.prefixSet_.data() + i * prefixLen_, prefixLen_);
      part.index_ = TerarkIndex::LoadMemory({indexMempry.data() + last.key,
        ptrdiff_t(curr.key - last.key)});
      part.store_.reset(BlobStore::load_from_user_memory({storeMemory.data() +
        last.value, ptrdiff_t(curr.value - last.value)}, dictMemory));
      assert(bitfield_array<2>::compute_mem_size(part.index_->NumKeys()) == curr.type - last.type);
      part.type_.risk_set_data((byte_t*)(typeMemory.data() + last.type), part.index_->NumKeys());
      part.commonPrefix_.assign(commonPrefixMemory.data() + last.commonPrefix,
        curr.commonPrefix - last.commonPrefix);
      last = curr;
    }
  }
  catch (const std::exception& ex) {
    segments_.clear();
    return Status::Corruption("TerarkZipTableReader::Open()", ex.what());
  }
  return Status::OK();
}

size_t TerarkZipTableMultiReader::SegmentIndex::GetSegmentCount() const {
  return partCount_;
}

const TerarkZipSegment* TerarkZipTableMultiReader::SegmentIndex::GetSegment(size_t i) const {
  return &segments_[i];
}

const TerarkZipSegment* TerarkZipTableMultiReader::SegmentIndex::GetSegment(fstring key) const {
  return (this->*get_segment_ptr)(key);
}

InternalIterator* TerarkZipTableMultiReader::NewIterator(const ReadOptions &,
  Arena *arena, bool skip_filters) {
  (void)skip_filters; // unused
  if (isReverseBytewiseOrder_) {
    if (arena) {
      return new(arena->AllocateAligned(sizeof(TerarkZipTableMultiIterator<true>)))
        TerarkZipTableMultiIterator<true>(table_reader_options_, segmentIndex_, global_seqno_);
    }
    else {
      return new TerarkZipTableMultiIterator<true>(table_reader_options_, segmentIndex_, global_seqno_);
    }
  }
  else {
    if (arena) {
      return new(arena->AllocateAligned(sizeof(TerarkZipTableMultiIterator<false>)))
        TerarkZipTableMultiIterator<false>(table_reader_options_, segmentIndex_, global_seqno_);
    }
    else {
      return new TerarkZipTableMultiIterator<false>(table_reader_options_, segmentIndex_, global_seqno_);
    }
  }
}

Status
TerarkZipTableMultiReader::Get(const ReadOptions& ro, const Slice& ikey,
  GetContext* get_context, bool skip_filters) {
  int flag = skip_filters ? TerarkZipSegment::FlagSkipFilter : TerarkZipSegment::FlagNone;
  ParsedInternalKey pikey;
  if (ikey.size() < 8 + pikey.user_key.size()) {
    return Status::InvalidArgument("TerarkZipTableMultiReader::Get()",
      "param target.size() < 8");
  }
  auto segment = segmentIndex_.GetSegment(fstringOf(ikey).substr(0, ikey.size() - 8));
  if (segment == nullptr) {
    Status::OK();
  }
  return segment->Get(global_seqno_, ro, ikey, get_context, flag);
}

TerarkZipTableMultiReader::~TerarkZipTableMultiReader() {
}

TerarkZipTableMultiReader::TerarkZipTableMultiReader(const TableReaderOptions& tro
  , const TerarkZipTableOptions& tzto)
  : table_reader_options_(tro)
  , global_seqno_(kDisableGlobalSequenceNumber)
  , tzto_(tzto)
{
  isReverseBytewiseOrder_ = false;
}

Status
rocksdb::TerarkZipTableMultiReader::Open(RandomAccessFileReader* file, uint64_t file_size) {
  file_.reset(file); // take ownership
  const auto& ioptions = table_reader_options_.ioptions;
  TableProperties* props = nullptr;
  Status s = ReadTableProperties(file, file_size,
    kTerarkZipTableMagicNumber, ioptions, &props);
  if (!s.ok()) {
    return s;
  }
  assert(nullptr != props);
  unique_ptr<TableProperties> uniqueProps(props);
  Slice file_data;
  if (table_reader_options_.env_options.use_mmap_reads) {
    s = file->Read(0, file_size, &file_data, nullptr);
    if (!s.ok())
      return s;
  }
  else {
    return Status::InvalidArgument("TerarkZipTableReader::Open()",
      "EnvOptions::use_mmap_reads must be true");
  }
  file_data_ = file_data;
  table_properties_.reset(uniqueProps.release());
  global_seqno_ = GetGlobalSequenceNumber(*table_properties_, ioptions.info_log);
  isReverseBytewiseOrder_ =
    fstring(ioptions.user_comparator->Name()).startsWith("rev:");
#if defined(TERARK_SUPPORT_UINT64_COMPARATOR) && BOOST_ENDIAN_LITTLE_BYTE
  assert(fstring(ioptions.user_comparator->Name()) != "rocksdb.Uint64Comparator");
#endif
  BlockContents valueDictBlock, indexBlock, zValueTypeBlock, commonPrefixBlock;
  BlockContents offsetBlock;
#if defined(TerocksPrivateCode)
  BlockContents licenseBlock;
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableExtendedBlock, &licenseBlock);
  if (s.ok()) {
    auto table_factory = dynamic_cast<TerarkZipTableFactory*>(ioptions.table_factory);
    assert(table_factory);
    auto& license = table_factory->GetLicense();
    auto res = license.merge(licenseBlock.data.data(), licenseBlock.data.size());
    assert(res == LicenseInfo::Result::OK);
    (void)res; // shut up !
    if (!license.check()) {
      license.print_error(nullptr, false, ioptions.info_log);
      return Status::Corruption("License expired", "contact@terark.com");
    }
  }
#endif // TerocksPrivateCode
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableOffsetBlock, &offsetBlock);
  if (!s.ok()) {
    return s;
  }
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableValueDictBlock, &valueDictBlock);
  if (!s.ok()) {
    return s;
  }
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableIndexBlock, &indexBlock);
  if (!s.ok()) {
    return s;
  }
  s = LoadTombstone(file, file_size);
  if (global_seqno_ == kDisableGlobalSequenceNumber) {
    global_seqno_ = 0;
  }
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableCommonPrefixBlock, &commonPrefixBlock);
  if (!s.ok()) {
    return s;
  }
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
    kTerarkZipTableValueTypeBlock, &zValueTypeBlock);
  if (!s.ok()) {
    return s;
  }
  s = segmentIndex_.Init(fstringOf(offsetBlock.data)
    , fstringOf(indexBlock.data)
    , fstring(file_data.data(), props->data_size)
    , fstringOf(valueDictBlock.data)
    , fstringOf(zValueTypeBlock.data)
    , fstringOf(commonPrefixBlock.data)
    , isReverseBytewiseOrder_
  );
  if (!s.ok()) {
    return s;
  }
  long long t0 = g_pf.now();

  if (tzto_.warmUpIndexOnOpen) {
    MmapWarmUp(fstringOf(indexBlock.data));
    if (!tzto_.warmUpValueOnOpen) {
      MmapWarmUp(fstringOf(valueDictBlock.data));
      for (size_t i = 0; i < segmentIndex_.GetSegmentCount(); ++i) {
        auto part = segmentIndex_.GetSegment(i);
        for (fstring block : part->store_->get_index_blocks()) {
          MmapWarmUp(block);
        }
      }
    }
  }
  if (tzto_.warmUpValueOnOpen) {
    MmapWarmUp(fstring(file_data.data(), props->data_size));
  }

  long long t1 = g_pf.now();
  size_t keyCount = 0;
  for (size_t i = 0; i < segmentIndex_.GetSegmentCount(); ++i) {
    auto part = segmentIndex_.GetSegment(i);
    part->index_->BuildCache(tzto_.indexCacheRatio);
    keyCount += part->index_->NumKeys();
  }
  long long t2 = g_pf.now();
  INFO(ioptions.info_log
    , "TerarkZipTableReader::Open(): fsize = %zd, entries = %zd keys = %zd indexSize = %zd valueSize=%zd, warm up time = %6.3f'sec, build cache time = %6.3f'sec\n"
    , size_t(file_size), size_t(table_properties_->num_entries)
    , keyCount
    , size_t(table_properties_->index_size)
    , size_t(table_properties_->data_size)
    , g_pf.sf(t0, t1)
    , g_pf.sf(t1, t2)
  );
  return Status::OK();
}

#endif // TerocksPrivateCode

}
