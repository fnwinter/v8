// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_SNAPSHOT_DESERIALIZER_H_
#define V8_SNAPSHOT_DESERIALIZER_H_

#include <utility>
#include <vector>

#include "src/execution/local-isolate-wrapper.h"
#include "src/objects/allocation-site.h"
#include "src/objects/api-callbacks.h"
#include "src/objects/backing-store.h"
#include "src/objects/code.h"
#include "src/objects/js-array.h"
#include "src/objects/map.h"
#include "src/objects/string-table.h"
#include "src/objects/string.h"
#include "src/snapshot/deserializer-allocator.h"
#include "src/snapshot/serializer-deserializer.h"
#include "src/snapshot/snapshot-source-sink.h"

namespace v8 {
namespace internal {

class HeapObject;
class Object;

// Used for platforms with embedded constant pools to trigger deserialization
// of objects found in code.
#if defined(V8_TARGET_ARCH_MIPS) || defined(V8_TARGET_ARCH_MIPS64) || \
    defined(V8_TARGET_ARCH_PPC) || defined(V8_TARGET_ARCH_S390) ||    \
    defined(V8_TARGET_ARCH_PPC64) || V8_EMBEDDED_CONSTANT_POOL
#define V8_CODE_EMBEDS_OBJECT_POINTER 1
#else
#define V8_CODE_EMBEDS_OBJECT_POINTER 0
#endif

// A Deserializer reads a snapshot and reconstructs the Object graph it defines.
class V8_EXPORT_PRIVATE Deserializer : public SerializerDeserializer {
 public:
  ~Deserializer() override;

  void SetRehashability(bool v) { can_rehash_ = v; }
  uint32_t GetChecksum() const { return source_.GetChecksum(); }

 protected:
  // Create a deserializer from a snapshot byte source.
  template <class Data>
  Deserializer(Data* data, bool deserializing_user_code)
      : local_isolate_(nullptr),
        source_(data->Payload()),
        magic_number_(data->GetMagicNumber()),
        deserializing_user_code_(deserializing_user_code),
        can_rehash_(false) {
    allocator()->DecodeReservation(data->Reservations());
    // We start the indices here at 1, so that we can distinguish between an
    // actual index and a nullptr in a deserialized object requiring fix-up.
    backing_stores_.push_back({});
  }

  void Initialize(Isolate* isolate) {
    Initialize(LocalIsolateWrapper(isolate));
  }
  void Initialize(LocalIsolateWrapper isolate);
  void DeserializeDeferredObjects();

  // Create Log events for newly deserialized objects.
  void LogNewObjectEvents();
  void LogScriptEvents(Script script);
  void LogNewMapEvents();

  // This returns the address of an object that has been described in the
  // snapshot by chunk index and offset.
  HeapObject GetBackReferencedObject(SnapshotSpace space);

  // Add an object to back an attached reference. The order to add objects must
  // mirror the order they are added in the serializer.
  void AddAttachedObject(Handle<HeapObject> attached_object) {
    attached_objects_.push_back(attached_object);
  }

  void CheckNoArrayBufferBackingStores() {
    CHECK_EQ(new_off_heap_array_buffers().size(), 0);
  }

  LocalIsolateWrapper local_isolate() const { return local_isolate_; }
  Isolate* isolate() const { return local_isolate().main_thread(); }
  bool is_main_thread() const { return local_isolate().is_main_thread(); }
  bool is_off_thread() const { return local_isolate().is_off_thread(); }

  SnapshotByteSource* source() { return &source_; }
  const std::vector<AllocationSite>& new_allocation_sites() const {
    return new_allocation_sites_;
  }
  const std::vector<Code>& new_code_objects() const {
    return new_code_objects_;
  }
  const std::vector<Map>& new_maps() const { return new_maps_; }
  const std::vector<AccessorInfo>& accessor_infos() const {
    return accessor_infos_;
  }
  const std::vector<CallHandlerInfo>& call_handler_infos() const {
    return call_handler_infos_;
  }
  const std::vector<Handle<Script>>& new_scripts() const {
    return new_scripts_;
  }

  const std::vector<Handle<JSArrayBuffer>>& new_off_heap_array_buffers() const {
    return new_off_heap_array_buffers_;
  }

  std::shared_ptr<BackingStore> backing_store(size_t i) {
    DCHECK_LT(i, backing_stores_.size());
    return backing_stores_[i];
  }

  DeserializerAllocator* allocator() { return &allocator_; }
  bool deserializing_user_code() const { return deserializing_user_code_; }
  bool can_rehash() const { return can_rehash_; }

  void Rehash();

 private:
  void VisitRootPointers(Root root, const char* description,
                         FullObjectSlot start, FullObjectSlot end) override;

  void Synchronize(VisitorSynchronization::SyncTag tag) override;

  template <typename TSlot>
  inline TSlot Write(TSlot dest, MaybeObject value);

  template <typename TSlot>
  inline TSlot WriteAddress(TSlot dest, Address value);

  template <typename TSlot>
  inline TSlot WriteExternalPointer(TSlot dest, Address value);

  // Fills in some heap data in an area from start to end (non-inclusive).  The
  // space id is used for the write barrier.  The object_address is the address
  // of the object we are writing into, or nullptr if we are not writing into an
  // object, i.e. if we are writing a series of tagged values that are not on
  // the heap. Return false if the object content has been deferred.
  template <typename TSlot>
  bool ReadData(TSlot start, TSlot end, SnapshotSpace space,
                Address object_address);

  // A helper function for ReadData, templatized on the bytecode for efficiency.
  // Returns the new value of {current}.
  template <typename TSlot, Bytecode bytecode,
            SnapshotSpace space_number_if_any>
  inline TSlot ReadDataCase(TSlot current, Address current_object_address,
                            byte data, bool write_barrier_needed);

  // A helper function for ReadData for reading external references.
  inline Address ReadExternalReferenceCase();

  HeapObject ReadObject(SnapshotSpace space_number);
  void ReadCodeObjectBody(SnapshotSpace space_number,
                          Address code_object_address);

 protected:
  HeapObject ReadObject();

 public:
  void VisitCodeTarget(Code host, RelocInfo* rinfo);
  void VisitEmbeddedPointer(Code host, RelocInfo* rinfo);
  void VisitRuntimeEntry(Code host, RelocInfo* rinfo);
  void VisitExternalReference(Code host, RelocInfo* rinfo);
  void VisitInternalReference(Code host, RelocInfo* rinfo);
  void VisitOffHeapTarget(Code host, RelocInfo* rinfo);

 private:
  template <typename TSlot>
  TSlot ReadRepeatedObject(TSlot current, int repeat_count);

  // Special handling for serialized code like hooking up internalized strings.
  HeapObject PostProcessNewObject(HeapObject obj, SnapshotSpace space);

  // Cached current isolate.
  LocalIsolateWrapper local_isolate_;

  // Objects from the attached object descriptions in the serialized user code.
  std::vector<Handle<HeapObject>> attached_objects_;

  SnapshotByteSource source_;
  uint32_t magic_number_;

  std::vector<Map> new_maps_;
  std::vector<AllocationSite> new_allocation_sites_;
  std::vector<Code> new_code_objects_;
  std::vector<AccessorInfo> accessor_infos_;
  std::vector<CallHandlerInfo> call_handler_infos_;
  std::vector<Handle<Script>> new_scripts_;
  std::vector<Handle<JSArrayBuffer>> new_off_heap_array_buffers_;
  std::vector<std::shared_ptr<BackingStore>> backing_stores_;

  DeserializerAllocator allocator_;
  const bool deserializing_user_code_;

  // TODO(6593): generalize rehashing, and remove this flag.
  bool can_rehash_;
  std::vector<HeapObject> to_rehash_;
  // Store the objects whose maps are deferred and thus initialized as filler
  // maps during deserialization, so that they can be processed later when the
  // maps become available.
  std::unordered_map<HeapObject, SnapshotSpace, Object::Hasher>
      fillers_to_post_process_;

#ifdef DEBUG
  uint32_t num_api_references_;
#endif  // DEBUG

  // For source(), isolate(), and allocator().
  friend class DeserializerAllocator;

  DISALLOW_COPY_AND_ASSIGN(Deserializer);
};

// Used to insert a deserialized internalized string into the string table.
class StringTableInsertionKey final : public StringTableKey {
 public:
  explicit StringTableInsertionKey(Handle<String> string);

  bool IsMatch(String string) override;

  V8_WARN_UNUSED_RESULT Handle<String> AsHandle(Isolate* isolate) override;

 private:
  uint32_t ComputeHashField(String string);

  Handle<String> string_;
  DISALLOW_HEAP_ALLOCATION(no_gc)
};

}  // namespace internal
}  // namespace v8

#endif  // V8_SNAPSHOT_DESERIALIZER_H_
