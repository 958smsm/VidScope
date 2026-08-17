#include "TestHarness.h"
#include "TestFrameFactory.h"

#include "playback/FrameQueue.h"

#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

VIDSCOPE_TEST(FrameQueue_enforces_item_and_byte_bounds)
{
    const auto first = vidscope::test::makeTestFrame(1, 0ns);
    const auto second = vidscope::test::makeTestFrame(2, 40ms);
    const auto third = vidscope::test::makeTestFrame(3, 80ms);
    const auto frameBytes = first->estimatedBytes();
    VIDSCOPE_REQUIRE(frameBytes > 0);

    vidscope::playback::FrameQueue queue(2, frameBytes * 2);
    VIDSCOPE_REQUIRE(queue.tryPush(first));
    VIDSCOPE_REQUIRE(queue.tryPush(second));
    VIDSCOPE_REQUIRE(!queue.tryPush(third));
    VIDSCOPE_REQUIRE(queue.size() == 2);
    VIDSCOPE_REQUIRE(queue.bytes() <= frameBytes * 2);

    const auto popped = queue.tryPop();
    VIDSCOPE_REQUIRE(popped.has_value());
    VIDSCOPE_REQUIRE((*popped)->id.sessionSerial == 1);
    VIDSCOPE_REQUIRE(queue.tryPush(third));
}

VIDSCOPE_TEST(FrameQueue_waiters_stop_deterministically)
{
    vidscope::playback::FrameQueue queue(2, 1U << 20U);
    std::promise<void> entered;
    std::promise<void> finished;
    auto enteredFuture = entered.get_future();
    auto finishedFuture = finished.get_future();
    bool receivedFrame = true;

    std::jthread waiter([&](std::stop_token stop) {
        entered.set_value();
        receivedFrame = queue.waitPop(stop).has_value();
        finished.set_value();
    });
    enteredFuture.wait();
    waiter.request_stop();
    finishedFuture.wait();
    VIDSCOPE_REQUIRE(!receivedFrame);
}

VIDSCOPE_TEST(FrameQueue_close_wakes_waiters)
{
    vidscope::playback::FrameQueue queue(1, 1U << 20U);
    std::promise<void> entered;
    std::promise<void> finished;
    auto enteredFuture = entered.get_future();
    auto finishedFuture = finished.get_future();
    bool receivedFrame = true;

    std::jthread waiter([&](std::stop_token stop) {
        entered.set_value();
        receivedFrame = queue.waitPop(stop).has_value();
        finished.set_value();
    });
    enteredFuture.wait();
    queue.close();
    finishedFuture.wait();
    VIDSCOPE_REQUIRE(!receivedFrame);
}
