#include <cppunit/ui/text/TestRunner.h>
#include <cppunit/extensions/HelperMacros.h>
#include "calc.h"

class GraphTest final : public CppUnit::TestFixture  {
public:
    const std::function<int(int)> int_identity = [](int a) { return a; };

    void testSingleNode() {
        struct calc::Stats stats;
        calc::Graph g;
        std::atomic<int> res(0);
        
        // setup
        auto node = g.node().connect(
            std::plus<int>(),
            calc::unconnected<int>(),
            calc::unconnected<int>());
        node->input<0>().append(g, 1);
        node->input<1>().append(g, 2);
        node->connect(calc::Input<int>(res));
        
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 1);
        CPPUNIT_ASSERT(res.load() == 3);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 0);
        CPPUNIT_ASSERT(stats.worked == 0);

        // update an input
        node->input<0>().append(g, 3);
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 1);
        CPPUNIT_ASSERT(res.load() == 5);
    }

    void testConstant() {
        struct calc::Stats stats;
        calc::Graph g;
        std::atomic<int> res;

        calc::Constant<int> one(1), two(2);
        auto node = g.node().connect(
            std::plus<int>(),
            &one,
            &two);
        node->connect(calc::Input<int>(res));
        
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 1);
        CPPUNIT_ASSERT(res.load() == 3);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 0);
        CPPUNIT_ASSERT(stats.worked == 0);
    }

    void testCircular() {
        struct calc::Stats stats;
        calc::Graph g;
        std::atomic<int> res(0);
        
        // setup: connect output to second input
        auto node = g.node().connect(
            std::plus<int>(),
            calc::unconnected<int>(),
            calc::unconnected<int>());
        node->input<0>().append(g, 1);
        node->connect(node->input<1>());
        node->connect(calc::Input<int>(res));
        
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 1);
        CPPUNIT_ASSERT(res.load() == 1);

        // should recycle input
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 1);
        CPPUNIT_ASSERT(res.load() == 2);

        // should recycle input again
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 1);
        CPPUNIT_ASSERT(res.load() == 3);

        // try updating the seed
        node->input<0>().append(g, 5);
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 1);
        CPPUNIT_ASSERT(res.load() == 8);

        // should recycle re-seeded input
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 1);
        CPPUNIT_ASSERT(res.load() == 9);
    }

    void testChain() {
        struct calc::Stats stats;
        calc::Graph g;
        std::atomic<bool> res;
        
        // setup
        auto in1 = g.node().connect(
            int_identity,
            calc::unconnected<int>());
        auto in2 = g.node().connect(
            int_identity,
            calc::unconnected<int>());
        auto out = g.node().connect(
            std::less<int>(),
            in1.get(),
            in2.get());
        out->connect(calc::Input<bool>(res));
        
        in1->input<0>().append(g, 1);
        in2->input<0>().append(g, 2);
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 3);
        CPPUNIT_ASSERT(stats.worked == 3);
        CPPUNIT_ASSERT(res.load() == true);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 0);
        CPPUNIT_ASSERT(stats.worked == 0);

        // update an input & check only one runs
        in1->input<0>().append(g, 3);
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 2);
        CPPUNIT_ASSERT(res.load() == false);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 0);
        CPPUNIT_ASSERT(stats.worked == 0);

        // update both inputs
        in1->input<0>().append(g, 5);
        in2->input<0>().append(g, 6);
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 2);
        CPPUNIT_ASSERT(stats.worked == 3);
        CPPUNIT_ASSERT(res.load() == true);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 0);
        CPPUNIT_ASSERT(stats.worked == 0);
    }


    void testUpdatePolicy() {
        struct calc::Stats stats;
        calc::Graph g;
        std::atomic<int> always_res, onchange_res;
        
        // setup
        auto in = g.node().connect(
            int_identity,
            calc::unconnected<int>());
        auto always = g.
            node().
            propagate<calc::Always>().
            connect(
                int_identity,
                in.get());
        auto afteralways = g.node().connect(int_identity, always.get());
        afteralways->connect(calc::Input<int>(always_res));
        auto onchange = g.
            node().
            propagate<calc::OnChange>().
            connect(
                int_identity,
                in.get());
        auto afteronchange = g.node().connect(int_identity, onchange.get());
        afteronchange->connect(calc::Input<int>(onchange_res));
        
        in->input<0>().append(g, 1);
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 5);
        CPPUNIT_ASSERT(stats.worked == 5);
        CPPUNIT_ASSERT(always_res.load() == 1);
        CPPUNIT_ASSERT(onchange_res.load() == 1);

        // check an empty run
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 0);
        CPPUNIT_ASSERT(stats.worked == 0);

        // same input
        in->input<0>().append(g, 1);
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 4); // *not* 5
        CPPUNIT_ASSERT(always_res.load() == 1);
        CPPUNIT_ASSERT(onchange_res.load() == 1);

        // a new input
        in->input<0>().append(g, 2);
        g(&stats);
        CPPUNIT_ASSERT(stats.queued == 1);
        CPPUNIT_ASSERT(stats.worked == 5);
        CPPUNIT_ASSERT(always_res.load() == 2);
        CPPUNIT_ASSERT(onchange_res.load() == 2);
    }

    CPPUNIT_TEST_SUITE(GraphTest);
    CPPUNIT_TEST(testSingleNode);
    CPPUNIT_TEST(testConstant);
    CPPUNIT_TEST(testChain);
    CPPUNIT_TEST(testUpdatePolicy);
    CPPUNIT_TEST_SUITE_END();
};

int main() {
    CppUnit::TextUi::TestRunner runner;
    runner.addTest( GraphTest::suite() );
    return !runner.run("", false);
}
