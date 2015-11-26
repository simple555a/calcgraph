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
            static_cast<calc::Connectable<int>*>(nullptr),
            static_cast<calc::Connectable<int>*>(nullptr));
        node->input<0>().append(g, 1);
        node->input<1>().append(g, 2);
        node->connect(calc::Input<int>(res));
        g();

        CPPUNIT_ASSERT(res.load() == 3);
    }

    CPPUNIT_TEST_SUITE(GraphTest);
    CPPUNIT_TEST(testSingleNode);
    CPPUNIT_TEST_SUITE_END();
};

int main() {
    CppUnit::TextUi::TestRunner runner;
    runner.addTest( GraphTest::suite() );
    return !runner.run("", false);
}
