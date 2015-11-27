#include <cppunit/ui/text/TestRunner.h>
#include <cppunit/extensions/HelperMacros.h>
#include "calc.h"

class GraphTest final : public CppUnit::TestFixture  {
public:

    void testSingleNode() {
        calc::Graph g;
        std::atomic<int> res;
        
        auto node = g.node(
            std::plus<int>(),
            calc::unconnected<int>(),
            calc::unconnected<int>());
        node->input<0>().append(g, 1);
        node->input<1>().append(g, 2);
        node->connect(calc::Input<int>(res));
        g();

        CPPUNIT_ASSERT(res.load() == 3);
    }

    void testConstant() {
        calc::Graph g;
        std::atomic<int> res;
        auto one = calc::Constant<int>(1);
        auto two = calc::Constant<int>(2);
        
        auto node = g.node(
            std::plus<int>(),
            &one,
            &two);
        node->connect(calc::Input<int>(res));
        g();

        CPPUNIT_ASSERT(res.load() == 3);
    }

    CPPUNIT_TEST_SUITE(GraphTest);
    CPPUNIT_TEST(testSingleNode);
    CPPUNIT_TEST(testConstant);
    CPPUNIT_TEST_SUITE_END();
};

int main() {
    CppUnit::TextUi::TestRunner runner;
    runner.addTest( GraphTest::suite() );
    return !runner.run("", false);
}
