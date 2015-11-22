#include <cppunit/ui/text/TestRunner.h>
#include <cppunit/extensions/HelperMacros.h>
#include "calc.cpp"

template<int GRAPH, typename INOUT>
class TestExtractor : public calc::Extractor<GRAPH, INOUT, INOUT> {
public:
    const INOUT snap() {
        return it.load();
    }
    void append(const INOUT& val, calc::WorkState<GRAPH>& ws) {
        it.store(val);
    }
private:
    std::atomic<INOUT> it;
};

class GraphTest final : public CppUnit::TestFixture  {
public:
    auto result() {
        return std::shared_ptr<calc::Extractor<0, int, int>>(new TestExtractor<0, int>());
    }

    void testInput() {
        calc::Graph<0> g;
        auto input = g.input(1);

        auto res = result();
        input->connect(res);
        g.eval();
        CPPUNIT_ASSERT(res->snap() == 1);
    }

    void testSimpleGraph() {
        calc::Graph<0> g;
        auto in1 = g.input(1);
        auto in2 = g.input(2);
        auto node = g.node(std::plus<int>(), in1, in2);

        auto res = result();
        node->connect(res);
        g.eval();
        CPPUNIT_ASSERT(res->snap() == 3);
    }

    CPPUNIT_TEST_SUITE(GraphTest);
    CPPUNIT_TEST(testInput);
    CPPUNIT_TEST(testSimpleGraph);
    CPPUNIT_TEST_SUITE_END();
};

int main() {
    CppUnit::TextUi::TestRunner runner;
    runner.addTest( GraphTest::suite() );
    return !runner.run("", false);
}
