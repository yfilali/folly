/*
 * Copyright 2014 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define __STDC_LIMIT_MACROS

#include "folly/io/IOBuf.h"

#include "folly/Conv.h"
#include "folly/Likely.h"
#include "folly/Malloc.h"
#include "folly/Memory.h"
#include "folly/ScopeGuard.h"
#include "folly/SpookyHashV2.h"
#include "folly/io/Cursor.h"

#include <stdexcept>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

using std::unique_ptr;

namespace {

enum : uint16_t {
  kHeapMagic = 0xa5a5,
  // This memory segment contains an IOBuf that is still in use
  kIOBufInUse = 0x01,
  // This memory segment contains buffer data that is still in use
  kDataInUse = 0x02,
};

enum : uint64_t {
  // When create() is called for buffers less than kDefaultCombinedBufSize,
  // we allocate a single combined memory segment for the IOBuf and the data
  // together.  See the comments for createCombined()/createSeparate() for more
  // details.
  //
  // (The size of 1k is largely just a guess here.  We could could probably do
  // benchmarks of real applications to see if adjusting this number makes a
  // difference.  Callers that know their exact use case can also explicitly
  // call createCombined() or createSeparate().)
  kDefaultCombinedBufSize = 1024
};

// Helper function for IOBuf::takeOwnership()
void takeOwnershipError(bool freeOnError, void* buf,
                        folly::IOBuf::FreeFunction freeFn,
                        void* userData) {
  if (!freeOnError) {
    return;
  }
  if (!freeFn) {
    free(buf);
    return;
  }
  try {
    freeFn(buf, userData);
  } catch (...) {
    // The user's free function is not allowed to throw.
    // (We are already in the middle of throwing an exception, so
    // we cannot let this exception go unhandled.)
    abort();
  }
}

} // unnamed namespace

namespace folly {

struct IOBuf::HeapPrefix {
  HeapPrefix(uint16_t flg)
    : magic(kHeapMagic),
      flags(flg) {}
  ~HeapPrefix() {
    // Reset magic to 0 on destruction.  This is solely for debugging purposes
    // to help catch bugs where someone tries to use HeapStorage after it has
    // been deleted.
    magic = 0;
  }

  uint16_t magic;
  std::atomic<uint16_t> flags;
};

struct IOBuf::HeapStorage {
  HeapPrefix prefix;
  // The IOBuf is last in the HeapStorage object.
  // This way operator new will work even if allocating a subclass of IOBuf
  // that requires more space.
  folly::IOBuf buf;
};

struct IOBuf::HeapFullStorage {
  // Make sure jemalloc allocates from the 64-byte class.  Putting this here
  // because HeapStorage is private so it can't be at namespace level.
  static_assert(sizeof(HeapStorage) <= 64,
                "IOBuf may not grow over 56 bytes!");

  HeapStorage hs;
  SharedInfo shared;
  MaxAlign align;
};

IOBuf::SharedInfo::SharedInfo()
  : freeFn(nullptr),
    userData(nullptr) {
  // Use relaxed memory ordering here.  Since we are creating a new SharedInfo,
  // no other threads should be referring to it yet.
  refcount.store(1, std::memory_order_relaxed);
}

IOBuf::SharedInfo::SharedInfo(FreeFunction fn, void* arg)
  : freeFn(fn),
    userData(arg) {
  // Use relaxed memory ordering here.  Since we are creating a new SharedInfo,
  // no other threads should be referring to it yet.
  refcount.store(1, std::memory_order_relaxed);
}

void* IOBuf::operator new(size_t size) {
  size_t fullSize = offsetof(HeapStorage, buf) + size;
  auto* storage = static_cast<HeapStorage*>(malloc(fullSize));
  // operator new is not allowed to return NULL
  if (UNLIKELY(storage == nullptr)) {
    throw std::bad_alloc();
  }

  new (&storage->prefix) HeapPrefix(kIOBufInUse);
  return &(storage->buf);
}

void* IOBuf::operator new(size_t size, void* ptr) {
  return ptr;
}

void IOBuf::operator delete(void* ptr) {
  auto* storageAddr = static_cast<uint8_t*>(ptr) - offsetof(HeapStorage, buf);
  auto* storage = reinterpret_cast<HeapStorage*>(storageAddr);
  releaseStorage(storage, kIOBufInUse);
}

void IOBuf::releaseStorage(HeapStorage* storage, uint16_t freeFlags) {
  CHECK_EQ(storage->prefix.magic, static_cast<uint16_t>(kHeapMagic));

  // Use relaxed memory order here.  If we are unlucky and happen to get
  // out-of-date data the compare_exchange_weak() call below will catch
  // it and load new data with memory_order_acq_rel.
  auto flags = storage->prefix.flags.load(std::memory_order_acquire);
  DCHECK_EQ((flags & freeFlags), freeFlags);

  while (true) {
    uint16_t newFlags = (flags & ~freeFlags);
    if (newFlags == 0) {
      // The storage space is now unused.  Free it.
      storage->prefix.HeapPrefix::~HeapPrefix();
      free(storage);
      return;
    }

    // This storage segment still contains portions that are in use.
    // Just clear the flags specified in freeFlags for now.
    auto ret = storage->prefix.flags.compare_exchange_weak(
        flags, newFlags, std::memory_order_acq_rel);
    if (ret) {
      // We successfully updated the flags.
      return;
    }

    // We failed to update the flags.  Some other thread probably updated them
    // and cleared some of the other bits.  Continue around the loop to see if
    // we are the last user now, or if we need to try updating the flags again.
  }
}

void IOBuf::freeInternalBuf(void* buf, void* userData) {
  auto* storage = static_cast<HeapStorage*>(userData);
  releaseStorage(storage, kDataInUse);
}

IOBuf::IOBuf(CreateOp, uint64_t capacity)
  : next_(this),
    prev_(this),
    data_(nullptr),
    length_(0),
    flagsAndSharedInfo_(0) {
  SharedInfo* info;
  allocExtBuffer(capacity, &buf_, &info, &capacity_);
  setSharedInfo(info);
  data_ = buf_;
}

IOBuf::IOBuf(CopyBufferOp op, const void* buf, uint64_t size,
             uint64_t headroom, uint64_t minTailroom)
  : IOBuf(CREATE, headroom + size + minTailroom) {
  advance(headroom);
  memcpy(writableData(), buf, size);
  append(size);
}

IOBuf::IOBuf(CopyBufferOp op, ByteRange br,
             uint64_t headroom, uint64_t minTailroom)
  : IOBuf(op, br.data(), br.size(), headroom, minTailroom) {
}

unique_ptr<IOBuf> IOBuf::create(uint64_t capacity) {
  // For smaller-sized buffers, allocate the IOBuf, SharedInfo, and the buffer
  // all with a single allocation.
  //
  // We don't do this for larger buffers since it can be wasteful if the user
  // needs to reallocate the buffer but keeps using the same IOBuf object.
  // In this case we can't free the data space until the IOBuf is also
  // destroyed.  Callers can explicitly call createCombined() or
  // createSeparate() if they know their use case better, and know if they are
  // likely to reallocate the buffer later.
  if (capacity <= kDefaultCombinedBufSize) {
    return createCombined(capacity);
  }
  return createSeparate(capacity);
}

unique_ptr<IOBuf> IOBuf::createCombined(uint64_t capacity) {
  // To save a memory allocation, allocate space for the IOBuf object, the
  // SharedInfo struct, and the data itself all with a single call to malloc().
  size_t requiredStorage = offsetof(HeapFullStorage, align) + capacity;
  size_t mallocSize = goodMallocSize(requiredStorage);
  auto* storage = static_cast<HeapFullStorage*>(malloc(mallocSize));

  new (&storage->hs.prefix) HeapPrefix(kIOBufInUse | kDataInUse);
  new (&storage->shared) SharedInfo(freeInternalBuf, storage);

  uint8_t* bufAddr = reinterpret_cast<uint8_t*>(&storage->align);
  uint8_t* storageEnd = reinterpret_cast<uint8_t*>(storage) + mallocSize;
  size_t actualCapacity = storageEnd - bufAddr;
  unique_ptr<IOBuf> ret(new (&storage->hs.buf) IOBuf(
        InternalConstructor(), packFlagsAndSharedInfo(0, &storage->shared),
        bufAddr, actualCapacity, bufAddr, 0));
  return ret;
}

unique_ptr<IOBuf> IOBuf::createSeparate(uint64_t capacity) {
  return make_unique<IOBuf>(CREATE, capacity);
}

unique_ptr<IOBuf> IOBuf::createChain(
    size_t totalCapacity, uint64_t maxBufCapacity) {
  unique_ptr<IOBuf> out = create(
      std::min(totalCapacity, size_t(maxBufCapacity)));
  size_t allocatedCapacity = out->capacity();

  while (allocatedCapacity < totalCapacity) {
    unique_ptr<IOBuf> newBuf = create(
        std::min(totalCapacity - allocatedCapacity, size_t(maxBufCapacity)));
    allocatedCapacity += newBuf->capacity();
    out->prependChain(std::move(newBuf));
  }

  return out;
}

IOBuf::IOBuf(TakeOwnershipOp, void* buf, uint64_t capacity, uint64_t length,
             FreeFunction freeFn, void* userData,
             bool freeOnError)
  : next_(this),
    prev_(this),
    data_(static_cast<uint8_t*>(buf)),
    buf_(static_cast<uint8_t*>(buf)),
    length_(length),
    capacity_(capacity),
    flagsAndSharedInfo_(packFlagsAndSharedInfo(kFlagFreeSharedInfo, nullptr)) {
  try {
    setSharedInfo(new SharedInfo(freeFn, userData));
  } catch (...) {
    takeOwnershipError(freeOnError, buf, freeFn, userData);
    throw;
  }
}

unique_ptr<IOBuf> IOBuf::takeOwnership(void* buf, uint64_t capacity,
                                       uint64_t length,
                                       FreeFunction freeFn,
                                       void* userData,
                                       bool freeOnError) {
  try {
    // TODO: We could allocate the IOBuf object and SharedInfo all in a single
    // memory allocation.  We could use the existing HeapStorage class, and
    // define a new kSharedInfoInUse flag.  We could change our code to call
    // releaseStorage(kFlagFreeSharedInfo) when this kFlagFreeSharedInfo,
    // rather than directly calling delete.
    //
    // Note that we always pass freeOnError as false to the constructor.
    // If the constructor throws we'll handle it below.  (We have to handle
    // allocation failures from make_unique too.)
    return make_unique<IOBuf>(TAKE_OWNERSHIP, buf, capacity, length,
                              freeFn, userData, false);
  } catch (...) {
    takeOwnershipError(freeOnError, buf, freeFn, userData);
    throw;
  }
}

IOBuf::IOBuf(WrapBufferOp, const void* buf, uint64_t capacity)
  : IOBuf(InternalConstructor(), 0,
          // We cast away the const-ness of the buffer here.
          // This is okay since IOBuf users must use unshare() to create a copy
          // of this buffer before writing to the buffer.
          static_cast<uint8_t*>(const_cast<void*>(buf)), capacity,
          static_cast<uint8_t*>(const_cast<void*>(buf)), capacity) {
}

IOBuf::IOBuf(WrapBufferOp op, ByteRange br)
  : IOBuf(op, br.data(), br.size()) {
}

unique_ptr<IOBuf> IOBuf::wrapBuffer(const void* buf, uint64_t capacity) {
  return make_unique<IOBuf>(WRAP_BUFFER, buf, capacity);
}

IOBuf::IOBuf() noexcept {
}

IOBuf::IOBuf(IOBuf&& other) noexcept {
  *this = std::move(other);
}

IOBuf::IOBuf(InternalConstructor,
             uintptr_t flagsAndSharedInfo,
             uint8_t* buf,
             uint64_t capacity,
             uint8_t* data,
             uint64_t length)
  : next_(this),
    prev_(this),
    data_(data),
    buf_(buf),
    length_(length),
    capacity_(capacity),
    flagsAndSharedInfo_(flagsAndSharedInfo) {
  assert(data >= buf);
  assert(data + length <= buf + capacity);
}

IOBuf::~IOBuf() {
  // Destroying an IOBuf destroys the entire chain.
  // Users of IOBuf should only explicitly delete the head of any chain.
  // The other elements in the chain will be automatically destroyed.
  while (next_ != this) {
    // Since unlink() returns unique_ptr() and we don't store it,
    // it will automatically delete the unlinked element.
    (void)next_->unlink();
  }

  decrementRefcount();
}

IOBuf& IOBuf::operator=(IOBuf&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  // If we are part of a chain, delete the rest of the chain.
  while (next_ != this) {
    // Since unlink() returns unique_ptr() and we don't store it,
    // it will automatically delete the unlinked element.
    (void)next_->unlink();
  }

  // Decrement our refcount on the current buffer
  decrementRefcount();

  // Take ownership of the other buffer's data
  data_ = other.data_;
  buf_ = other.buf_;
  length_ = other.length_;
  capacity_ = other.capacity_;
  flagsAndSharedInfo_ = other.flagsAndSharedInfo_;
  // Reset other so it is a clean state to be destroyed.
  other.data_ = nullptr;
  other.buf_ = nullptr;
  other.length_ = 0;
  other.capacity_ = 0;
  other.flagsAndSharedInfo_ = 0;

  // If other was part of the chain, assume ownership of the rest of its chain.
  // (It's only valid to perform move assignment on the head of a chain.)
  if (other.next_ != &other) {
    next_ = other.next_;
    next_->prev_ = this;
    other.next_ = &other;

    prev_ = other.prev_;
    prev_->next_ = this;
    other.prev_ = &other;
  }

  // Sanity check to make sure that other is in a valid state to be destroyed.
  DCHECK_EQ(other.prev_, &other);
  DCHECK_EQ(other.next_, &other);

  return *this;
}

bool IOBuf::empty() const {
  const IOBuf* current = this;
  do {
    if (current->length() != 0) {
      return false;
    }
    current = current->next_;
  } while (current != this);
  return true;
}

size_t IOBuf::countChainElements() const {
  size_t numElements = 1;
  for (IOBuf* current = next_; current != this; current = current->next_) {
    ++numElements;
  }
  return numElements;
}

uint64_t IOBuf::computeChainDataLength() const {
  uint64_t fullLength = length_;
  for (IOBuf* current = next_; current != this; current = current->next_) {
    fullLength += current->length_;
  }
  return fullLength;
}

void IOBuf::prependChain(unique_ptr<IOBuf>&& iobuf) {
  // Take ownership of the specified IOBuf
  IOBuf* other = iobuf.release();

  // Remember the pointer to the tail of the other chain
  IOBuf* otherTail = other->prev_;

  // Hook up prev_->next_ to point at the start of the other chain,
  // and other->prev_ to point at prev_
  prev_->next_ = other;
  other->prev_ = prev_;

  // Hook up otherTail->next_ to point at us,
  // and prev_ to point back at otherTail,
  otherTail->next_ = this;
  prev_ = otherTail;
}

unique_ptr<IOBuf> IOBuf::clone() const {
  unique_ptr<IOBuf> ret = make_unique<IOBuf>();
  cloneInto(*ret);
  return ret;
}

unique_ptr<IOBuf> IOBuf::cloneOne() const {
  unique_ptr<IOBuf> ret = make_unique<IOBuf>();
  cloneOneInto(*ret);
  return ret;
}

void IOBuf::cloneInto(IOBuf& other) const {
  IOBuf tmp;
  cloneOneInto(tmp);

  for (IOBuf* current = next_; current != this; current = current->next_) {
    tmp.prependChain(current->cloneOne());
  }

  other = std::move(tmp);
}

void IOBuf::cloneOneInto(IOBuf& other) const {
  SharedInfo* info = sharedInfo();
  if (info) {
    setFlags(kFlagMaybeShared);
  }
  other = IOBuf(InternalConstructor(),
                flagsAndSharedInfo_, buf_, capacity_,
                data_, length_);
  if (info) {
    info->refcount.fetch_add(1, std::memory_order_acq_rel);
  }
}

void IOBuf::unshareOneSlow() {
  // Allocate a new buffer for the data
  uint8_t* buf;
  SharedInfo* sharedInfo;
  uint64_t actualCapacity;
  allocExtBuffer(capacity_, &buf, &sharedInfo, &actualCapacity);

  // Copy the data
  // Maintain the same amount of headroom.  Since we maintained the same
  // minimum capacity we also maintain at least the same amount of tailroom.
  uint64_t headlen = headroom();
  memcpy(buf + headlen, data_, length_);

  // Release our reference on the old buffer
  decrementRefcount();
  // Make sure kFlagMaybeShared and kFlagFreeSharedInfo are all cleared.
  setFlagsAndSharedInfo(0, sharedInfo);

  // Update the buffer pointers to point to the new buffer
  data_ = buf + headlen;
  buf_ = buf;
}

void IOBuf::unshareChained() {
  // unshareChained() should only be called if we are part of a chain of
  // multiple IOBufs.  The caller should have already verified this.
  assert(isChained());

  IOBuf* current = this;
  while (true) {
    if (current->isSharedOne()) {
      // we have to unshare
      break;
    }

    current = current->next_;
    if (current == this) {
      // None of the IOBufs in the chain are shared,
      // so return without doing anything
      return;
    }
  }

  // We have to unshare.  Let coalesceSlow() do the work.
  coalesceSlow();
}

void IOBuf::coalesceSlow() {
  // coalesceSlow() should only be called if we are part of a chain of multiple
  // IOBufs.  The caller should have already verified this.
  DCHECK(isChained());

  // Compute the length of the entire chain
  uint64_t newLength = 0;
  IOBuf* end = this;
  do {
    newLength += end->length_;
    end = end->next_;
  } while (end != this);

  coalesceAndReallocate(newLength, end);
  // We should be only element left in the chain now
  DCHECK(!isChained());
}

void IOBuf::coalesceSlow(size_t maxLength) {
  // coalesceSlow() should only be called if we are part of a chain of multiple
  // IOBufs.  The caller should have already verified this.
  DCHECK(isChained());
  DCHECK_LT(length_, maxLength);

  // Compute the length of the entire chain
  uint64_t newLength = 0;
  IOBuf* end = this;
  while (true) {
    newLength += end->length_;
    end = end->next_;
    if (newLength >= maxLength) {
      break;
    }
    if (end == this) {
      throw std::overflow_error("attempted to coalesce more data than "
                                "available");
    }
  }

  coalesceAndReallocate(newLength, end);
  // We should have the requested length now
  DCHECK_GE(length_, maxLength);
}

void IOBuf::coalesceAndReallocate(size_t newHeadroom,
                                  size_t newLength,
                                  IOBuf* end,
                                  size_t newTailroom) {
  uint64_t newCapacity = newLength + newHeadroom + newTailroom;
  if (newCapacity > UINT32_MAX) {
    throw std::overflow_error("IOBuf chain too large to coalesce");
  }

  // Allocate space for the coalesced buffer.
  // We always convert to an external buffer, even if we happened to be an
  // internal buffer before.
  uint8_t* newBuf;
  SharedInfo* newInfo;
  uint64_t actualCapacity;
  allocExtBuffer(newCapacity, &newBuf, &newInfo, &actualCapacity);

  // Copy the data into the new buffer
  uint8_t* newData = newBuf + newHeadroom;
  uint8_t* p = newData;
  IOBuf* current = this;
  size_t remaining = newLength;
  do {
    assert(current->length_ <= remaining);
    remaining -= current->length_;
    memcpy(p, current->data_, current->length_);
    p += current->length_;
    current = current->next_;
  } while (current != end);
  assert(remaining == 0);

  // Point at the new buffer
  decrementRefcount();

  // Make sure kFlagMaybeShared and kFlagFreeSharedInfo are all cleared.
  setFlagsAndSharedInfo(0, newInfo);

  capacity_ = actualCapacity;
  buf_ = newBuf;
  data_ = newData;
  length_ = newLength;

  // Separate from the rest of our chain.
  // Since we don't store the unique_ptr returned by separateChain(),
  // this will immediately delete the returned subchain.
  if (isChained()) {
    (void)separateChain(next_, current->prev_);
  }
}

void IOBuf::decrementRefcount() {
  // Externally owned buffers don't have a SharedInfo object and aren't managed
  // by the reference count
  SharedInfo* info = sharedInfo();
  if (!info) {
    return;
  }

  // Decrement the refcount
  uint32_t newcnt = info->refcount.fetch_sub(
      1, std::memory_order_acq_rel);
  // Note that fetch_sub() returns the value before we decremented.
  // If it is 1, we were the only remaining user; if it is greater there are
  // still other users.
  if (newcnt > 1) {
    return;
  }

  // We were the last user.  Free the buffer
  freeExtBuffer();

  // Free the SharedInfo if it was allocated separately.
  //
  // This is only used by takeOwnership().
  //
  // To avoid this special case handling in decrementRefcount(), we could have
  // takeOwnership() set a custom freeFn() that calls the user's free function
  // then frees the SharedInfo object.  (This would require that
  // takeOwnership() store the user's free function with its allocated
  // SharedInfo object.)  However, handling this specially with a flag seems
  // like it shouldn't be problematic.
  if (flags() & kFlagFreeSharedInfo) {
    delete sharedInfo();
  }
}

void IOBuf::reserveSlow(uint64_t minHeadroom, uint64_t minTailroom) {
  size_t newCapacity = (size_t)length_ + minHeadroom + minTailroom;
  DCHECK_LT(newCapacity, UINT32_MAX);

  // reserveSlow() is dangerous if anyone else is sharing the buffer, as we may
  // reallocate and free the original buffer.  It should only ever be called if
  // we are the only user of the buffer.
  DCHECK(!isSharedOne());

  // We'll need to reallocate the buffer.
  // There are a few options.
  // - If we have enough total room, move the data around in the buffer
  //   and adjust the data_ pointer.
  // - If we're using an internal buffer, we'll switch to an external
  //   buffer with enough headroom and tailroom.
  // - If we have enough headroom (headroom() >= minHeadroom) but not too much
  //   (so we don't waste memory), we can try one of two things, depending on
  //   whether we use jemalloc or not:
  //   - If using jemalloc, we can try to expand in place, avoiding a memcpy()
  //   - If not using jemalloc and we don't have too much to copy,
  //     we'll use realloc() (note that realloc might have to copy
  //     headroom + data + tailroom, see smartRealloc in folly/Malloc.h)
  // - Otherwise, bite the bullet and reallocate.
  if (headroom() + tailroom() >= minHeadroom + minTailroom) {
    uint8_t* newData = writableBuffer() + minHeadroom;
    memmove(newData, data_, length_);
    data_ = newData;
    return;
  }

  size_t newAllocatedCapacity = goodExtBufferSize(newCapacity);
  uint8_t* newBuffer = nullptr;
  uint64_t newHeadroom = 0;
  uint64_t oldHeadroom = headroom();

  // If we have a buffer allocated with malloc and we just need more tailroom,
  // try to use realloc()/rallocm() to grow the buffer in place.
  SharedInfo* info = sharedInfo();
  if (info && (info->freeFn == nullptr) && length_ != 0 &&
      oldHeadroom >= minHeadroom) {
    if (usingJEMalloc()) {
      size_t headSlack = oldHeadroom - minHeadroom;
      // We assume that tailroom is more useful and more important than
      // headroom (not least because realloc / rallocm allow us to grow the
      // buffer at the tail, but not at the head)  So, if we have more headroom
      // than we need, we consider that "wasted".  We arbitrarily define "too
      // much" headroom to be 25% of the capacity.
      if (headSlack * 4 <= newCapacity) {
        size_t allocatedCapacity = capacity() + sizeof(SharedInfo);
        void* p = buf_;
        if (allocatedCapacity >= jemallocMinInPlaceExpandable) {
          // rallocm can write to its 2nd arg even if it returns
          // ALLOCM_ERR_NOT_MOVED. So, we pass a temporary to its 2nd arg and
          // update newAllocatedCapacity only on success.
          size_t allocatedSize;
          int r = rallocm(&p, &allocatedSize, newAllocatedCapacity,
                          0, ALLOCM_NO_MOVE);
          if (r == ALLOCM_SUCCESS) {
            newBuffer = static_cast<uint8_t*>(p);
            newHeadroom = oldHeadroom;
            newAllocatedCapacity = allocatedSize;
          } else if (r == ALLOCM_ERR_OOM) {
            // shouldn't happen as we don't actually allocate new memory
            // (due to ALLOCM_NO_MOVE)
            throw std::bad_alloc();
          }
          // if ALLOCM_ERR_NOT_MOVED, do nothing, fall back to
          // malloc/memcpy/free
        }
      }
    } else {  // Not using jemalloc
      size_t copySlack = capacity() - length_;
      if (copySlack * 2 <= length_) {
        void* p = realloc(buf_, newAllocatedCapacity);
        if (UNLIKELY(p == nullptr)) {
          throw std::bad_alloc();
        }
        newBuffer = static_cast<uint8_t*>(p);
        newHeadroom = oldHeadroom;
      }
    }
  }

  // None of the previous reallocation strategies worked (or we're using
  // an internal buffer).  malloc/copy/free.
  if (newBuffer == nullptr) {
    void* p = malloc(newAllocatedCapacity);
    if (UNLIKELY(p == nullptr)) {
      throw std::bad_alloc();
    }
    newBuffer = static_cast<uint8_t*>(p);
    memcpy(newBuffer + minHeadroom, data_, length_);
    if (sharedInfo()) {
      freeExtBuffer();
    }
    newHeadroom = minHeadroom;
  }

  uint64_t cap;
  initExtBuffer(newBuffer, newAllocatedCapacity, &info, &cap);

  if (flags() & kFlagFreeSharedInfo) {
    delete sharedInfo();
  }

  setFlagsAndSharedInfo(0, info);
  capacity_ = cap;
  buf_ = newBuffer;
  data_ = newBuffer + newHeadroom;
  // length_ is unchanged
}

void IOBuf::freeExtBuffer() {
  SharedInfo* info = sharedInfo();
  DCHECK(info);

  if (info->freeFn) {
    try {
      info->freeFn(buf_, info->userData);
    } catch (...) {
      // The user's free function should never throw.  Otherwise we might
      // throw from the IOBuf destructor.  Other code paths like coalesce()
      // also assume that decrementRefcount() cannot throw.
      abort();
    }
  } else {
    free(buf_);
  }
}

void IOBuf::allocExtBuffer(uint64_t minCapacity,
                           uint8_t** bufReturn,
                           SharedInfo** infoReturn,
                           uint64_t* capacityReturn) {
  size_t mallocSize = goodExtBufferSize(minCapacity);
  uint8_t* buf = static_cast<uint8_t*>(malloc(mallocSize));
  if (UNLIKELY(buf == nullptr)) {
    throw std::bad_alloc();
  }
  initExtBuffer(buf, mallocSize, infoReturn, capacityReturn);
  *bufReturn = buf;
}

size_t IOBuf::goodExtBufferSize(uint64_t minCapacity) {
  // Determine how much space we should allocate.  We'll store the SharedInfo
  // for the external buffer just after the buffer itself.  (We store it just
  // after the buffer rather than just before so that the code can still just
  // use free(buf_) to free the buffer.)
  size_t minSize = static_cast<size_t>(minCapacity) + sizeof(SharedInfo);
  // Add room for padding so that the SharedInfo will be aligned on an 8-byte
  // boundary.
  minSize = (minSize + 7) & ~7;

  // Use goodMallocSize() to bump up the capacity to a decent size to request
  // from malloc, so we can use all of the space that malloc will probably give
  // us anyway.
  return goodMallocSize(minSize);
}

void IOBuf::initExtBuffer(uint8_t* buf, size_t mallocSize,
                          SharedInfo** infoReturn,
                          uint64_t* capacityReturn) {
  // Find the SharedInfo storage at the end of the buffer
  // and construct the SharedInfo.
  uint8_t* infoStart = (buf + mallocSize) - sizeof(SharedInfo);
  SharedInfo* sharedInfo = new(infoStart) SharedInfo;

  *capacityReturn = infoStart - buf;
  *infoReturn = sharedInfo;
}

fbstring IOBuf::moveToFbString() {
  // malloc-allocated buffers are just fine, everything else needs
  // to be turned into one.
  if (!sharedInfo() ||         // user owned, not ours to give up
      sharedInfo()->freeFn ||  // not malloc()-ed
      headroom() != 0 ||       // malloc()-ed block doesn't start at beginning
      tailroom() == 0 ||       // no room for NUL terminator
      isShared() ||            // shared
      isChained()) {           // chained
    // We might as well get rid of all head and tailroom if we're going
    // to reallocate; we need 1 byte for NUL terminator.
    coalesceAndReallocate(0, computeChainDataLength(), this, 1);
  }

  // Ensure NUL terminated
  *writableTail() = 0;
  fbstring str(reinterpret_cast<char*>(writableData()),
               length(),  capacity(),
               AcquireMallocatedString());

  if (flags() & kFlagFreeSharedInfo) {
    delete sharedInfo();
  }

  // Reset to a state where we can be deleted cleanly
  flagsAndSharedInfo_ = 0;
  buf_ = nullptr;
  clear();
  return str;
}

IOBuf::Iterator IOBuf::cbegin() const {
  return Iterator(this, this);
}

IOBuf::Iterator IOBuf::cend() const {
  return Iterator(nullptr, nullptr);
}

folly::fbvector<struct iovec> IOBuf::getIov() const {
  folly::fbvector<struct iovec> iov;
  iov.reserve(countChainElements());
  IOBuf const* p = this;
  do {
    // some code can get confused by empty iovs, so skip them
    if (p->length() > 0) {
      iov.push_back({(void*)p->data(), folly::to<size_t>(p->length())});
    }
    p = p->next();
  } while (p != this);
  return iov;
}

size_t IOBufHash::operator()(const IOBuf& buf) const {
  folly::hash::SpookyHashV2 hasher;
  hasher.Init(0, 0);
  io::Cursor cursor(&buf);
  for (;;) {
    auto p = cursor.peek();
    if (p.second == 0) {
      break;
    }
    hasher.Update(p.first, p.second);
    cursor.skip(p.second);
  }
  uint64_t h1;
  uint64_t h2;
  hasher.Final(&h1, &h2);
  return h1;
}

bool IOBufEqual::operator()(const IOBuf& a, const IOBuf& b) const {
  io::Cursor ca(&a);
  io::Cursor cb(&b);
  for (;;) {
    auto pa = ca.peek();
    auto pb = cb.peek();
    if (pa.second == 0 && pb.second == 0) {
      return true;
    } else if (pa.second == 0 || pb.second == 0) {
      return false;
    }
    size_t n = std::min(pa.second, pb.second);
    DCHECK_GT(n, 0);
    if (memcmp(pa.first, pb.first, n)) {
      return false;
    }
    ca.skip(n);
    cb.skip(n);
  }
}

} // folly
