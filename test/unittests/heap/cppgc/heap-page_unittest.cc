// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/cppgc/heap-page.h"

#include <algorithm>

#include "include/cppgc/allocation.h"
#include "include/cppgc/internal/accessors.h"
#include "include/cppgc/persistent.h"
#include "src/base/macros.h"
#include "src/heap/cppgc/globals.h"
#include "src/heap/cppgc/heap-object-header-inl.h"
#include "src/heap/cppgc/heap-object-header.h"
#include "src/heap/cppgc/page-memory-inl.h"
#include "src/heap/cppgc/raw-heap.h"

#include "test/unittests/heap/cppgc/tests.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cppgc {
namespace internal {

namespace {

class PageTest : public testing::TestWithHeap {
 public:
  RawHeap& GetRawHeap() { return Heap::From(GetHeap())->raw_heap(); }
};

template <size_t Size>
class GCed : public GarbageCollected<GCed<Size>> {
 public:
  virtual void Trace(cppgc::Visitor*) const {}
  char array[Size];
};

}  // namespace

TEST_F(PageTest, GetHeapForAllocatedObject) {
  auto* gced = MakeGarbageCollected<GCed<1>>(GetHeap());
  EXPECT_EQ(GetHeap(), GetHeapFromPayload(gced));
}

TEST_F(PageTest, SpaceIndexing) {
  RawHeap& heap = GetRawHeap();
  size_t space = 0u;
  for (const auto& ptr : heap) {
    EXPECT_EQ(ptr.get(),
              heap.Space(static_cast<cppgc::Heap::SpaceType>(space)));
    EXPECT_EQ(&heap, ptr.get()->raw_heap());
    EXPECT_EQ(space, ptr->index());
    ++space;
  }
  EXPECT_EQ(space, static_cast<size_t>(cppgc::Heap::SpaceType::kUserDefined1));
}

TEST_F(PageTest, PredefinedSpaces) {
  using SpaceType = RawHeap::SpaceType;
  RawHeap& heap = GetRawHeap();
  {
    auto* gced = MakeGarbageCollected<GCed<1>>(GetHeap());
    BaseSpace* space = NormalPage::FromPayload(gced)->space();
    EXPECT_EQ(heap.Space(SpaceType::kNormal1), space);
    EXPECT_EQ(0u, space->index());
    EXPECT_FALSE(space->is_large());
  }
  {
    auto* gced = MakeGarbageCollected<GCed<32>>(GetHeap());
    BaseSpace* space = NormalPage::FromPayload(gced)->space();
    EXPECT_EQ(heap.Space(SpaceType::kNormal2), space);
    EXPECT_EQ(1u, space->index());
    EXPECT_FALSE(space->is_large());
  }
  {
    auto* gced = MakeGarbageCollected<GCed<64>>(GetHeap());
    BaseSpace* space = NormalPage::FromPayload(gced)->space();
    EXPECT_EQ(heap.Space(SpaceType::kNormal3), space);
    EXPECT_EQ(2u, space->index());
    EXPECT_FALSE(space->is_large());
  }
  {
    auto* gced = MakeGarbageCollected<GCed<128>>(GetHeap());
    BaseSpace* space = NormalPage::FromPayload(gced)->space();
    EXPECT_EQ(heap.Space(SpaceType::kNormal4), space);
    EXPECT_EQ(3u, space->index());
    EXPECT_FALSE(space->is_large());
  }
  {
    auto* gced =
        MakeGarbageCollected<GCed<2 * kLargeObjectSizeThreshold>>(GetHeap());
    BaseSpace* space = NormalPage::FromPayload(gced)->space();
    EXPECT_EQ(heap.Space(SpaceType::kLarge), space);
    EXPECT_EQ(4u, space->index());
    EXPECT_TRUE(space->is_large());
  }
}

TEST_F(PageTest, NormalPageIndexing) {
  using SpaceType = RawHeap::SpaceType;
  constexpr size_t kExpectedNumberOfPages = 10u;
  constexpr size_t kObjectSize = 8u;
  using Type = GCed<kObjectSize>;
  static const size_t kNumberOfObjects =
      (kExpectedNumberOfPages * NormalPage::PayloadSize() /
       (sizeof(Type) + sizeof(HeapObjectHeader)));

  std::vector<Persistent<Type>> persistents(kNumberOfObjects);
  for (auto& p : persistents) {
    p = MakeGarbageCollected<Type>(GetHeap());
  }

  const RawHeap& heap = GetRawHeap();
  const BaseSpace* space = heap.Space(SpaceType::kNormal1);
  EXPECT_EQ(kExpectedNumberOfPages, space->size());

  size_t page_n = 0;
  for (const BasePage* page : *space) {
    EXPECT_FALSE(page->is_large());
    EXPECT_EQ(space, page->space());
    ++page_n;
  }
  EXPECT_EQ(page_n, space->size());
}

TEST_F(PageTest, LargePageIndexing) {
  using SpaceType = RawHeap::SpaceType;
  constexpr size_t kExpectedNumberOfPages = 10u;
  constexpr size_t kObjectSize = 2 * kLargeObjectSizeThreshold;
  using Type = GCed<kObjectSize>;
  const size_t kNumberOfObjects = kExpectedNumberOfPages;

  std::vector<Persistent<Type>> persistents(kNumberOfObjects);
  for (auto& p : persistents) {
    p = MakeGarbageCollected<Type>(GetHeap());
  }

  const RawHeap& heap = GetRawHeap();
  const BaseSpace* space = heap.Space(SpaceType::kLarge);
  EXPECT_EQ(kExpectedNumberOfPages, space->size());

  size_t page_n = 0;
  for (const BasePage* page : *space) {
    EXPECT_TRUE(page->is_large());
    ++page_n;
  }
  EXPECT_EQ(page_n, space->size());
}

TEST_F(PageTest, HeapObjectHeaderOnBasePageIndexing) {
  constexpr size_t kObjectSize = 8;
  using Type = GCed<kObjectSize>;
  const size_t kNumberOfObjects =
      NormalPage::PayloadSize() / (sizeof(Type) + sizeof(HeapObjectHeader));
  const size_t kLeftSpace =
      NormalPage::PayloadSize() % (sizeof(Type) + sizeof(HeapObjectHeader));

  std::vector<Persistent<Type>> persistents(kNumberOfObjects);
  for (auto& p : persistents) {
    p = MakeGarbageCollected<Type>(GetHeap());
  }

  const auto* page =
      static_cast<NormalPage*>(BasePage::FromPayload(persistents[0].Get()));
  size_t size = 0;
  size_t num = 0;
  for (const HeapObjectHeader& header : *page) {
    EXPECT_EQ(reinterpret_cast<Address>(persistents[num].Get()),
              header.Payload());
    size += header.GetSize();
    ++num;
  }
  EXPECT_EQ(num, persistents.size());
  EXPECT_EQ(size + kLeftSpace, NormalPage::PayloadSize());
}

TEST_F(PageTest, HeapObjectHeaderOnLargePageIndexing) {
  constexpr size_t kObjectSize = 2 * kLargeObjectSizeThreshold;
  using Type = GCed<kObjectSize>;
  auto* gced = MakeGarbageCollected<Type>(GetHeap());

  const auto* page = static_cast<LargePage*>(BasePage::FromPayload(gced));
  const size_t expected_payload_size =
      RoundUp(sizeof(Type) + sizeof(HeapObjectHeader), kAllocationGranularity);
  EXPECT_EQ(expected_payload_size, page->PayloadSize());

  const HeapObjectHeader* header = page->ObjectHeader();
  EXPECT_EQ(reinterpret_cast<Address>(gced), header->Payload());
}

TEST_F(PageTest, NormalPageCreationDestruction) {
  RawHeap& heap = GetRawHeap();
  const PageBackend* backend = Heap::From(GetHeap())->page_backend();
  auto* space =
      static_cast<NormalPageSpace*>(heap.Space(RawHeap::SpaceType::kNormal1));
  auto* page = NormalPage::Create(space);
  EXPECT_NE(space->end(), std::find(space->begin(), space->end(), page));
  EXPECT_TRUE(
      space->free_list().Contains({page->PayloadStart(), page->PayloadSize()}));
  EXPECT_NE(nullptr, backend->Lookup(page->PayloadStart()));

  space->free_list().Clear();
  EXPECT_FALSE(
      space->free_list().Contains({page->PayloadStart(), page->PayloadSize()}));
  space->RemovePage(page);
  EXPECT_EQ(space->end(), std::find(space->begin(), space->end(), page));
  NormalPage::Destroy(page);
  EXPECT_EQ(nullptr, backend->Lookup(page->PayloadStart()));
}

TEST_F(PageTest, LargePageCreationDestruction) {
  constexpr size_t kObjectSize = 2 * kLargeObjectSizeThreshold;
  RawHeap& heap = GetRawHeap();
  const PageBackend* backend = Heap::From(GetHeap())->page_backend();
  auto* space =
      static_cast<LargePageSpace*>(heap.Space(RawHeap::SpaceType::kLarge));
  auto* page = LargePage::Create(space, kObjectSize);
  EXPECT_NE(space->end(), std::find(space->begin(), space->end(), page));
  EXPECT_NE(nullptr, backend->Lookup(page->PayloadStart()));

  space->RemovePage(page);
  EXPECT_EQ(space->end(), std::find(space->begin(), space->end(), page));
  LargePage::Destroy(page);
  EXPECT_EQ(nullptr, backend->Lookup(page->PayloadStart()));
}

#if DEBUG
TEST_F(PageTest, UnsweptPageDestruction) {
  RawHeap& heap = GetRawHeap();
  {
    auto* space =
        static_cast<NormalPageSpace*>(heap.Space(RawHeap::SpaceType::kNormal1));
    auto* page = NormalPage::Create(space);
    EXPECT_DEATH_IF_SUPPORTED(NormalPage::Destroy(page), "");
  }
  {
    auto* space =
        static_cast<LargePageSpace*>(heap.Space(RawHeap::SpaceType::kLarge));
    auto* page = LargePage::Create(space, 2 * kLargeObjectSizeThreshold);
    EXPECT_DEATH_IF_SUPPORTED(LargePage::Destroy(page), "");
  }
}
#endif

}  // namespace internal
}  // namespace cppgc
