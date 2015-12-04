#include <cppunit/ui/text/TestRunner.h>
#include <cppunit/extensions/HelperMacros.h>
#include <chrono>
#include <thread>

#include "calcgraph.h"

class GraphTest final : public CppUnit::TestFixture {
    using intlist = std::shared_ptr<std::forward_list<int>>;

  public:
    const std::function<int(int)> int_identity = [](int a) { return a; };
    const std::function<intlist(intlist)> intlist_identity =
        [](intlist a) { return a; };

    void testSingleNode() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<int> res;

        // setup
        auto node =
            g.node().connect(std::plus<int>(), calcgraph::unconnected<int>(),
                             calcgraph::unconnected<int>());
        node->input<0>().append(g, 1);
        node->input<1>().append(g, 2);
        node->connect(calcgraph::Input<int>(res));

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 3);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        // update an input
        node->input<0>().append(g, 3);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 5);
    }

    void testConstant() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<int> res;

        calcgraph::Constant<int> one(1), two(2);
        auto node = g.node().connect(std::plus<int>(), &one, &two);
        node->connect(calcgraph::Input<int>(res));

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 3);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);
    }

    void testCircular() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<int> res;

        // setup: connect output to second input
        auto node =
            g.node().connect(std::plus<int>(), calcgraph::unconnected<int>(),
                             calcgraph::unconnected<int>());
        node->input<0>().append(g, 1);
        node->connect(node->input<1>());
        node->connect(calcgraph::Input<int>(res));

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 1);

        // should recycle input
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 2);

        // should recycle input again
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 3);

        // try updating the seed
        node->input<0>().append(g, 5);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 8);

        // should recycle re-seeded input
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 9);
    }

    void testChain() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<bool> res;

        // setup
        auto in1 =
            g.node().connect(int_identity, calcgraph::unconnected<int>());
        auto in2 =
            g.node().connect(int_identity, calcgraph::unconnected<int>());
        auto out = g.node().connect(std::less<int>(), in1.get(), in2.get());
        out->connect(calcgraph::Input<bool>(res));

        in1->input<0>().append(g, 1);
        in2->input<0>().append(g, 2);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 3);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 3);
        CPPUNIT_ASSERT(res.read() == true);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        // update an input & check only one runs
        in1->input<0>().append(g, 3);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 2);
        CPPUNIT_ASSERT(res.read() == false);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        // update both inputs
        in1->input<0>().append(g, 5);
        in2->input<0>().append(g, 6);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 2);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 3);
        CPPUNIT_ASSERT(res.read() == true);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);
    }

    void testUpdatePolicy() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<int> always_res, onchange_res;

        // setup
        auto in = g.node().connect(int_identity, calcgraph::unconnected<int>());
        auto always = g.node().propagate<calcgraph::Always>().connect(
            int_identity, in.get());
        auto afteralways = g.node().connect(int_identity, always.get());
        afteralways->connect(calcgraph::Input<int>(always_res));
        auto onchange = g.node().propagate<calcgraph::OnChange>().connect(
            int_identity, in.get());
        auto afteronchange = g.node().connect(int_identity, onchange.get());
        afteronchange->connect(calcgraph::Input<int>(onchange_res));

        in->input<0>().append(g, 1);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 5);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 5);
        CPPUNIT_ASSERT(always_res.read() == 1);
        CPPUNIT_ASSERT(onchange_res.read() == 1);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        // same input
        in->input<0>().append(g, 1);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 4); // *not* 5
        CPPUNIT_ASSERT(always_res.read() == 1);
        CPPUNIT_ASSERT(onchange_res.read() == 1);

        // a new input
        in->input<0>().append(g, 2);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 5);
        CPPUNIT_ASSERT(always_res.read() == 2);
        CPPUNIT_ASSERT(onchange_res.read() == 2);
    }

    void testSharedPointer() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<std::size_t> res;
        calcgraph::Constant<std::shared_ptr<std::vector<int>>> it(
            std::shared_ptr<std::vector<int>>(new std::vector<int>()));

        // setup
        auto adder =
            g.node().connect([](std::shared_ptr<std::vector<int>> arr, int v) {
                arr->push_back(v);
                return arr;
            }, &it, calcgraph::unconnected<int>());
        auto sizer =
            g.node().connect([](std::shared_ptr<std::vector<int>> arr) {
                return arr->size();
            }, adder.get());
        sizer->connect(calcgraph::Input<std::size_t>(res));

        adder->input<1>().append(g, 1);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 2);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 2);
        CPPUNIT_ASSERT(res.read() == 1);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        adder->input<1>().append(g, 5);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 2);
        CPPUNIT_ASSERT(res.read() == 2);
    }

    void testThreaded() {
        calcgraph::Graph g;
        std::atomic<bool> stop(false);
        calcgraph::Latest<int> res;

        // start the evaluation thread
        std::thread t(calcgraph::evaluate_repeatedly, std::ref(g),
                      std::ref(stop));

        // setup
        auto node =
            g.node().connect(std::plus<int>(), calcgraph::unconnected<int>(),
                             calcgraph::unconnected<int>());
        node->input<0>().append(g, 1);
        node->input<1>().append(g, 2);
        node->connect(calcgraph::Input<int>(res));

        // ... wait for calculation
        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CPPUNIT_ASSERT(res.read() == 3);

        node->input<0>().append(g, 3);

        // ... wait for calculation
        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CPPUNIT_ASSERT(res.read() == 5);

        // terminate the evaluation thread
        stop.store(true, std::memory_order_seq_cst);
        t.join();
    }

    void testDisconnect() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<int> res;

        // setup
        auto node =
            g.node().connect(int_identity, calcgraph::unconnected<int>());
        node->input<0>().append(g, 1);
        node->connect(calcgraph::Input<int>(res));

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 1);

        // update an input
        node->input<0>().append(g, 3);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 3);

        node->disconnect(calcgraph::Input<int>(res));

        // update an input, again
        node->input<0>().append(g, 5);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 3);
    }

    template <typename RET>
    inline std::shared_ptr<std::vector<RET>>
    vector(std::initializer_list<RET> elements) {
        auto ret = std::shared_ptr<std::vector<RET>>(new std::vector<RET>());
        for (auto elem : elements) {
            ret->push_back(elem);
        }
        return ret;
    }

    void testAccumulator() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<std::shared_ptr<std::forward_list<int>>> res;

        // setup
        auto acc = g.node()
                       .accumulate(calcgraph::unconnected<int>())
                       .connect(intlist_identity);
        acc->input<0>().append(g, 3);
        acc->connect(
            calcgraph::Input<std::shared_ptr<std::forward_list<int>>>(res));

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        auto expected = vector({3});
        CPPUNIT_ASSERT(std::equal(expected->begin(), expected->end(),
                                  res.read()->begin(), res.read()->end()));

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        acc->input<0>().append(g, 5);
        acc->input<0>().append(g, 6);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        expected = vector({5, 6});
        CPPUNIT_ASSERT(std::equal(expected->begin(), expected->end(),
                                  res.read()->begin(), res.read()->end()));
    }

    CPPUNIT_TEST_SUITE(GraphTest);
    CPPUNIT_TEST(testSingleNode);
    CPPUNIT_TEST(testConstant);
    CPPUNIT_TEST(testChain);
    CPPUNIT_TEST(testUpdatePolicy);
    CPPUNIT_TEST(testSharedPointer);
    CPPUNIT_TEST(testDisconnect);
    CPPUNIT_TEST(testThreaded);
    CPPUNIT_TEST(testAccumulator);
    CPPUNIT_TEST_SUITE_END();
};

int main() {
    CppUnit::TextUi::TestRunner runner;
    runner.addTest(GraphTest::suite());
    return !runner.run("", false);
}
