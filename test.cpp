#include <cppunit/ui/text/TestRunner.h>
#include <cppunit/extensions/HelperMacros.h>
#include <chrono>
#include <thread>

#include "calcgraph.h"

class GraphTest final : public CppUnit::TestFixture {
    using intlist = std::shared_ptr<std::forward_list<int>>;
    using intvector = std::shared_ptr<std::vector<int>>;
    using intpair = std::pair<int, int>;
    using p_intpair = std::shared_ptr<intpair>;

  public:
    const std::function<int(int)> int_identity = [](int a) { return a; };
    const std::function<intlist(intlist)> intlist_identity =
        [](intlist a) { return a; };
    const std::function<intvector(intvector)> intvector_identity =
        [](intvector a) { return a; };

    void testAsserts() {
        static_assert(std::is_move_constructible<calcgraph::Input<int>>::value,
                      "not move constructible");
        static_assert(std::is_move_assignable<calcgraph::Input<int>>::value,
                      "not move assignable");
    }

    /**
     * @brief using NodeBuilder.connect to pass args
     */
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

    /**
     * @brief using NodeBuilder.latest to pass args
     */
    void testSingleNodeExplicit() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<int> res;

        // setup
        auto node = g.node()
                        .latest(calcgraph::unconnected<int>())
                        .latest(calcgraph::unconnected<int>())
                        .connect(std::plus<int>());
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

        auto node =
            g.node().initialize(1).initialize(2).connect(std::plus<int>());
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

    void testPropagationPolicies() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<int> always_res, onchange_res, weak_res;

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

        auto weak = g.node().propagate<calcgraph::Weak>().connect(int_identity,
                                                                  in.get());
        auto afterweak = g.node().connect(int_identity, weak.get());
        afterweak->connect(calcgraph::Input<int>(weak_res));

        in->input<0>().append(g, 1);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 7);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 7);
        CPPUNIT_ASSERT(always_res.read() == 1);
        CPPUNIT_ASSERT(onchange_res.read() == 1);
        CPPUNIT_ASSERT(weak_res.read() == 1);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        // same input
        in->input<0>().append(g, 1);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(
            stats, stats.worked ==
                       5); // *not* 7, afterweak & afteronchange not calculated
        CPPUNIT_ASSERT(always_res.read() == 1);
        CPPUNIT_ASSERT(onchange_res.read() == 1);
        CPPUNIT_ASSERT(weak_res.read() == 1);

        // a new input
        in->input<0>().append(g, 2);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 6);
        CPPUNIT_ASSERT(always_res.read() == 2);
        CPPUNIT_ASSERT(onchange_res.read() == 2);
        CPPUNIT_ASSERT(weak_res.read() == 1); // afterweak not calculated
    }

    void testSharedPointer() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<std::size_t> res;

        // setup
        auto adder = g.node()
                         .initialize(intvector(new std::vector<int>()))
                         .connect([](intvector arr, int v) {
                             arr->push_back(v);
                             return arr;
                         }, calcgraph::unconnected<int>());
        auto sizer = g.node().connect([](intvector arr) { return arr->size(); },
                                      adder.get());
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

    void testAccumulator() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<intlist> res;

        // setup
        auto acc = g.node()
                       .accumulate(calcgraph::unconnected<int>())
                       .connect(intlist_identity);
        acc->input<0>().append(g, 3);
        acc->connect(calcgraph::Input<intlist>(res));

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        auto expected = intvector(new std::vector<int>({3}));
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
        expected = intvector(new std::vector<int>({5, 6}));
        CPPUNIT_ASSERT(std::equal(expected->begin(), expected->end(),
                                  res.read()->begin(), res.read()->end()));
    }

    void testVariadic() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<intvector> res;

        // setup
        auto var = g.node().variadic<int>().connect(intvector_identity);
        auto one = var->variadic_add<0>();
        auto two = var->variadic_add<0>();
        var->connect(calcgraph::Input<intvector>(res));

        one.append(g, 5);
        two.append(g, 7);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        auto expected = intvector(new std::vector<int>({5, 7}));
        CPPUNIT_ASSERT(std::equal(expected->begin(), expected->end(),
                                  res.read()->begin(), res.read()->end()));

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        one.append(g, 2);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        expected = intvector(new std::vector<int>({2, 7}));
        CPPUNIT_ASSERT(std::equal(expected->begin(), expected->end(),
                                  res.read()->begin(), res.read()->end()));

        var->variadic_remove<0>(one);

        two.append(g, 4);
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        expected = intvector(new std::vector<int>({4}));
        CPPUNIT_ASSERT(std::equal(expected->begin(), expected->end(),
                                  res.read()->begin(), res.read()->end()));
    }

    void testMultiplexed() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<p_intpair> res;

        // setup
        auto node = g.node()
                        .output<calcgraph::Multiplexed>()
                        .latest(calcgraph::unconnected<p_intpair>())
                        .connect([](p_intpair a) { return *a; });
        node->connect(calcgraph::Input<p_intpair>(res));

        // test un-keyed inputs

        node->input<0>().append(g, p_intpair(new intpair(5, 7)));
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(*res.read() == intpair(5, 7));

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        node->input<0>().append(g, p_intpair(new intpair(3, 2)));
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(*res.read() == intpair(3, 2));

        // test keyed inputs
        calcgraph::Latest<int> one, two;
        node->keyed_output(1).connect(calcgraph::Input<int>(one));
        node->keyed_output(2).connect(calcgraph::Input<int>(two));

        node->input<0>().append(g, p_intpair(new intpair(1, 5)));
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(one.read() == 5);

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        node->input<0>().append(g, p_intpair(new intpair(2, 9)));
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(two.read() == 9);

        node->keyed_output(2).disconnect(calcgraph::Input<int>(two));

        node->input<0>().append(g, p_intpair(new intpair(2, 4)));
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(two.read() == 9); // *not* 4
    }

    void testMultiValued() {
        struct calcgraph::Stats stats;
        calcgraph::Graph g;
        calcgraph::Latest<int> res;

        // setup
        auto node =
            g.node()
                .output<calcgraph::MultiValued<calcgraph::SingleList>::type>()
                .latest(calcgraph::unconnected<intvector>())
                .connect(intvector_identity);
        node->connect(calcgraph::Input<int>(res));

        node->input<0>().append(g, intvector(new std::vector<int>({3, 5, 7})));
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 7);

        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 0);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 0);

        node->input<0>().append(g, intvector(new std::vector<int>({4, 3, 2})));
        g(&stats);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.queued == 1);
        CPPUNIT_ASSERT_MESSAGE(stats, stats.worked == 1);
        CPPUNIT_ASSERT(res.read() == 2);
    }

    /**
     * @brief Check the Node scheduled on the work queue is cleaned up by the
     * Graph's destructor
     */
    void testMemoryLeak() {
        calcgraph::Graph g;
        g.node()
            .output<calcgraph::MultiValued<calcgraph::SingleList>::type>()
            .latest(calcgraph::unconnected<intvector>())
            .connect(intvector_identity);
    }

    CPPUNIT_TEST_SUITE(GraphTest);
    CPPUNIT_TEST(testAsserts);
    CPPUNIT_TEST(testSingleNode);
    CPPUNIT_TEST(testSingleNodeExplicit);
    CPPUNIT_TEST(testConstant);
    CPPUNIT_TEST(testChain);
    CPPUNIT_TEST(testPropagationPolicies);
    CPPUNIT_TEST(testSharedPointer);
    CPPUNIT_TEST(testDisconnect);
    CPPUNIT_TEST(testThreaded);
    CPPUNIT_TEST(testAccumulator);
    CPPUNIT_TEST(testVariadic);
    CPPUNIT_TEST(testMultiplexed);
    CPPUNIT_TEST(testMultiValued);
    CPPUNIT_TEST(testMemoryLeak);
    CPPUNIT_TEST_SUITE_END();
};

int main() {
    CppUnit::TextUi::TestRunner runner;
    runner.addTest(GraphTest::suite());
    return !runner.run("", false);
}
