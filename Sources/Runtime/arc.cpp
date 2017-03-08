//
//  arc.cpp
//  trill
//
//  Created by Harlan Haskins on 3/6/17.
//  Copyright © 2017 Harlan. All rights reserved.
//

#include "arc.h"
#include "trill.h"
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <iostream>
#include <sstream>
#include <chrono>

#define DEBUG_ARC

#ifdef DEBUG_ARC
#define DEBUG_ARC_LOG(x) do { \
   auto time = std::chrono::system_clock::now(); \
   auto timeVal = std::chrono::system_clock::to_time_t(time); \
   std::cout << std::ctime(&timeVal) << "  " << x << " (" << value \
             << ") -- retain count is now " << box()->retainCount << std::endl; \
} while (false)
#else
#define DEBUG_ARC_LOG(x) ({})
#endif

namespace trill {

/**
 A RefCountBox contains
  - A retain count
  - A pointer to the type's deinitializer
  - A mutex used to synchronize retains/releases
  - A variably-sized payload that is not represented by a data member.

  It is meant as a hidden store for retain count data alongside the allocated
  contents of an indirect type.

  Trill will always see this as:
      [box reference (RefCountBox *)][payload (void *)]
                                     ^~ indirect type "begins" here
 */
struct RefCountBox {
  uint32_t retainCount;
  trill_deinitializer_t deinit;
  std::mutex mutex;

  RefCountBox(uint32_t retainCount, trill_deinitializer_t deinit):
    retainCount(retainCount), deinit(deinit) {}
};

/**
 A convenience struct that performs the arithmetic necessary to work with a
 \c RefCountBox.
 */
struct RefCounted {
public:
  RefCountBox **boxPtr;
  void *value;

  /**
   Creates a \c RefCounted along with a payload of the specific size.
   @param size The size of the underlying payload.
   @param deinit The deinitializer for the type being created.
   */
  RefCounted(size_t size, trill_deinitializer_t deinit) {
    auto boxFullPtr = trill_alloc(sizeof(RefCountBox *) + size);
    boxPtr = reinterpret_cast<RefCountBox **>(boxFullPtr);
    *boxPtr = new RefCountBox(0, deinit);
    value = reinterpret_cast<void *>(
              reinterpret_cast<uintptr_t>(boxFullPtr) + sizeof(boxPtr));
    DEBUG_ARC_LOG("creating box");
  }

  /**
   Gets a \c RefCounted instance for a given pointer into a box, by offsetting
   the value pointer with the size of the \c RefCountBox.
   
   @param boxedValue The payload value underlying a \c RefCountBox.
   */
  RefCounted(void *_Nonnull boxedValue) {
    value = boxedValue;
    boxPtr = reinterpret_cast<RefCountBox **>(
               reinterpret_cast<uintptr_t>(value) - sizeof(RefCountBox **));;
  }

  /**
   Reaches into the payload to find the reference counted box.
   */
  RefCountBox *box() {
    return *boxPtr;
  }

  void checkForUseAfterDealloc() {
    if (box() == nullptr) {
      std::stringstream msg;
      msg << "object (" << value << ") used after deallocation";
      trill_fatalError(msg.str().c_str());
    }
  }

  /**
   Determines if this object's reference count is exactly one.
   */
  bool isUniquelyReferenced() {
    checkForUseAfterDealloc();
    std::lock_guard<std::mutex> guard(box()->mutex);
    return box()->retainCount == 1;
  }

  /**
   Gets the current retain count of an object.
   */
  uint32_t retainCount() {
    checkForUseAfterDealloc();
    std::lock_guard<std::mutex> guard(box()->mutex);
    DEBUG_ARC_LOG("getting retain count");
    return box()->retainCount;
  }

  /**
   Retains the value inside a \c RefCountBox.
   */
  void retain() {
    checkForUseAfterDealloc();
    DEBUG_ARC_LOG(value);
    std::lock_guard<std::mutex> guard(box()->mutex);
    if (box()->retainCount == std::numeric_limits<decltype(box()->retainCount)>::max()) {
      trill_fatalError("retain count overflow");
    }
    box()->retainCount++;
    DEBUG_ARC_LOG("retaining object");
  }

  /**
   Releases the value inside a \c RefCountBox. If the value hits zero when
   this method is called, then the object will be explicitly deallocated.
   */
  void release() {
    checkForUseAfterDealloc();
    box()->mutex.lock();
    if (box()->retainCount == 0) {
      trill_fatalError("attempting to release object with retain count 0");
    }

    box()->retainCount--;

    DEBUG_ARC_LOG("releasing object");

    if (box()->retainCount == 0) {
      dealloc(); // mutex will be unlocked and invalidated
    } else {
      box()->mutex.unlock(); // otherwise manually unlock
    }
  }

private:
  /**
   Deallocates the value inside a \c RefCountBox.
   @note This function *must* be called with a locked \c mutex.
   */
  void dealloc() {
    checkForUseAfterDealloc();

    if (box()->retainCount > 0) {
      trill_fatalError("object deallocated with retain count > 0");
    }

    DEBUG_ARC_LOG("deallocating");

    if (box()->deinit != nullptr) {
      box()->deinit(value);
    }

    // Explicitly unlock the mutex before deleting it
    box()->mutex.unlock();
    delete box();

    // Erase the box's location
    *boxPtr = nullptr;
  }
};

void *_Nonnull trill_allocateIndirectType(size_t size,
                                          trill_deinitializer_t deinit) {
  return RefCounted(size, deinit).value;
}

void trill_retain(void *_Nonnull instance) {
  auto refCounted = RefCounted(instance);
  refCounted.retain();
}

void trill_release(void *_Nonnull instance) {
  auto refCounted = RefCounted(instance);
  refCounted.release();
}

uint8_t trill_isUniquelyReferenced(void *_Nonnull instance) {
  auto refCounted = RefCounted(instance);
  return refCounted.isUniquelyReferenced() ? 1 : 0;
}

}