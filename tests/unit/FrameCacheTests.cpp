#include "TestHarness.h"
#include "TestFrameFactory.h"

#include "playback/FrameCache.h"

#include <chrono>
#include <memory>

using namespace std::chrono_literals;

VIDSCOPE_TEST(FrameCache_evicts_lru_frames_but_preserves_the_pin)
{
    const auto one = vidscope::test::makeTestFrame(1, 0ms, true);
    const auto two = vidscope::test::makeTestFrame(2, 40ms);
    const auto three = vidscope::test::makeTestFrame(3, 80ms);
    const auto four = vidscope::test::makeTestFrame(4, 120ms, true);
    const auto bytes = one->estimatedBytes();
    VIDSCOPE_REQUIRE(bytes > 0);

    vidscope::playback::FrameCache cache(bytes * 3);
    VIDSCOPE_REQUIRE(cache.insert(one));
    VIDSCOPE_REQUIRE(cache.insert(two));
    VIDSCOPE_REQUIRE(cache.insert(three));
    cache.pin(2);
    VIDSCOPE_REQUIRE(cache.find(1) != nullptr); // Refresh frame one, making frame three LRU.
    VIDSCOPE_REQUIRE(cache.insert(four));

    VIDSCOPE_REQUIRE(cache.find(1) != nullptr);
    VIDSCOPE_REQUIRE(cache.find(2) != nullptr);
    VIDSCOPE_REQUIRE(cache.find(3) == nullptr);
    VIDSCOPE_REQUIRE(cache.find(4) != nullptr);
    VIDSCOPE_REQUIRE(cache.stats().bytes <= cache.stats().byteBudget);
    VIDSCOPE_REQUIRE(cache.stats().evictions >= 1);
}

VIDSCOPE_TEST(FrameCache_navigation_uses_presentation_order_not_dts_or_nominal_fps)
{
    const auto first = vidscope::test::makeTestFrame(11, 0ms, true);
    const auto second = vidscope::test::makeTestFrame(12, 33ms);
    const auto third = vidscope::test::makeTestFrame(13, 117ms, true);
    vidscope::playback::FrameCache cache(first->estimatedBytes() * 4);
    VIDSCOPE_REQUIRE(cache.insert(third));
    VIDSCOPE_REQUIRE(cache.insert(first));
    VIDSCOPE_REQUIRE(cache.insert(second));

    VIDSCOPE_REQUIRE(cache.previous(*third)->id.sessionSerial == second->id.sessionSerial);
    VIDSCOPE_REQUIRE(cache.next(*first)->id.sessionSerial == second->id.sessionSerial);
    VIDSCOPE_REQUIRE(cache.previousKeyframe(*third)->id.sessionSerial == first->id.sessionSerial);
    const auto following = cache.framesAfter(*first, 8);
    VIDSCOPE_REQUIRE(following.size() == 2);
    VIDSCOPE_REQUIRE(following[0]->id.sessionSerial == second->id.sessionSerial);
    VIDSCOPE_REQUIRE(following[1]->id.sessionSerial == third->id.sessionSerial);
}

VIDSCOPE_TEST(FrameCache_navigation_does_not_jump_across_an_evicted_middle_frame)
{
    const auto first = vidscope::test::makeTestFrame(1, 0ms);
    const auto middle = vidscope::test::makeTestFrame(2, 40ms);
    const auto third = vidscope::test::makeTestFrame(3, 80ms);
    const auto bytes = first->estimatedBytes();

    vidscope::playback::FrameCache cache(bytes * 2);
    VIDSCOPE_REQUIRE(cache.insert(first));
    VIDSCOPE_REQUIRE(cache.insert(middle));
    VIDSCOPE_REQUIRE(cache.find(first->id.sessionSerial) != nullptr); // Make the middle frame LRU.
    VIDSCOPE_REQUIRE(cache.insert(third));

    VIDSCOPE_REQUIRE(cache.find(middle->id.sessionSerial) == nullptr);
    VIDSCOPE_REQUIRE(cache.next(*first) == nullptr);
    VIDSCOPE_REQUIRE(cache.previous(*third) == nullptr);
}

VIDSCOPE_TEST(FrameCache_duplicate_times_have_a_stable_serial_total_order)
{
    auto first = std::const_pointer_cast<vidscope::media::DecodedFrame>(
        vidscope::test::makeTestFrame(10, 50ms));
    auto second = std::const_pointer_cast<vidscope::media::DecodedFrame>(
        vidscope::test::makeTestFrame(11, 50ms, true));
    auto third = std::const_pointer_cast<vidscope::media::DecodedFrame>(
        vidscope::test::makeTestFrame(12, 50ms));

    // Deliberately make index/PTS order disagree and mix known with unknown indices.
    first->id.presentationIndex = 100;
    first->id.pts = 900;
    second->id.presentationIndex = 50;
    second->id.pts = 800;
    third->id.presentationIndex = -1;
    third->id.pts = 700;

    vidscope::playback::FrameCache cache(first->estimatedBytes() * 4);
    VIDSCOPE_REQUIRE(cache.insert(third));
    VIDSCOPE_REQUIRE(cache.insert(second));
    VIDSCOPE_REQUIRE(cache.insert(first));

    const auto following = cache.framesAfter(*first, 8);
    VIDSCOPE_REQUIRE(following.size() == 2);
    VIDSCOPE_REQUIRE(following[0]->id.sessionSerial == second->id.sessionSerial);
    VIDSCOPE_REQUIRE(following[1]->id.sessionSerial == third->id.sessionSerial);
    VIDSCOPE_REQUIRE(cache.previousKeyframe(*third)->id.sessionSerial == second->id.sessionSerial);
}

VIDSCOPE_TEST(FrameCache_navigation_requires_compatible_adjacent_identities)
{
    const auto indexedCurrent = vidscope::test::makeTestFrame(20, 0ms);
    auto unindexedCandidate = std::const_pointer_cast<vidscope::media::DecodedFrame>(
        vidscope::test::makeTestFrame(21, 40ms));
    unindexedCandidate->id.presentationIndex = -1;

    vidscope::playback::FrameCache mixedCache(indexedCurrent->estimatedBytes() * 2);
    VIDSCOPE_REQUIRE(mixedCache.insert(unindexedCandidate));
    VIDSCOPE_REQUIRE(mixedCache.next(*indexedCurrent) == nullptr);

    auto unindexedCurrent = std::const_pointer_cast<vidscope::media::DecodedFrame>(
        vidscope::test::makeTestFrame(30, 0ms));
    auto unindexedNext = std::const_pointer_cast<vidscope::media::DecodedFrame>(
        vidscope::test::makeTestFrame(31, 40ms));
    unindexedCurrent->id.presentationIndex = -1;
    unindexedNext->id.presentationIndex = -1;

    vidscope::playback::FrameCache serialCache(unindexedNext->estimatedBytes() * 2);
    VIDSCOPE_REQUIRE(serialCache.insert(unindexedNext));
    VIDSCOPE_REQUIRE(serialCache.next(*unindexedCurrent)->id.sessionSerial == 31);
    VIDSCOPE_REQUIRE(serialCache.insert(unindexedCurrent));
    VIDSCOPE_REQUIRE(serialCache.previous(*unindexedNext)->id.sessionSerial == 30);
}

VIDSCOPE_TEST(FrameCache_rejects_a_surface_larger_than_its_budget)
{
    const auto frame = vidscope::test::makeTestFrame(1, 0ms, false, 512, 512);
    VIDSCOPE_REQUIRE(frame->estimatedBytes() > 1);
    vidscope::playback::FrameCache cache(frame->estimatedBytes() - 1);
    VIDSCOPE_REQUIRE(!cache.insert(frame));
    VIDSCOPE_REQUIRE(cache.stats().bytes == 0);
}

VIDSCOPE_TEST(FrameCache_rejects_zero_estimated_byte_entries)
{
    auto empty = std::make_shared<vidscope::media::DecodedFrame>();
    empty->id.sessionSerial = 99;
    empty->id.presentationIndex = 0;
    VIDSCOPE_REQUIRE(empty->estimatedBytes() == 0);

    vidscope::playback::FrameCache cache(1024);
    VIDSCOPE_REQUIRE(!cache.insert(empty));
    VIDSCOPE_REQUIRE(cache.stats().frameCount == 0);
    VIDSCOPE_REQUIRE(cache.stats().bytes == 0);
}
