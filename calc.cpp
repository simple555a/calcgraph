#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <forward_list>
#include <memory>
#include <queue>
#include <thread>
#include <tuple>

namespace calc {
    template<int GRAPH>
    class Graph;
    template<int GRAPH>
    class Work;
    template<int GRAPH, typename OUT>
    class Evaluation;

    template<int GRAPH>
    struct WorkQueueCmp {
        constexpr bool operator()(const Work<GRAPH>* a, const Work<GRAPH>* b) {
            return b->id > a->id;
        }
    };

    template<int GRAPH>
    class WorkState final {
    public:
        void add_to_queue(Work<GRAPH>& work) {
            if (work.id <= current_id)
                // process it next Graph#eval
                g.add_to_queue(work);
            else
                // spin-lock it and add to our queue
                q.push(&work);
        }

        void eval() {
            auto w = q.top();
            q.pop();
            current_id = w->id;
            w->eval(*this);
        }
    private:
        std::priority_queue<
            Work<GRAPH>*,
            std::vector<Work<GRAPH>*>,
            WorkQueueCmp<GRAPH>> q;
        Graph<GRAPH>& g;
        friend class Graph<GRAPH>;
        uint32_t current_id;
        WorkState(Graph<GRAPH>& g) : g(g) {}
    };

    template<int GRAPH, typename IN>
    class Appendable {
    public:
        virtual void append(const IN&, WorkState<GRAPH>&) = 0;
    };

    template<typename OUT>
    class Snappable {
    public:
        virtual const OUT snap() = 0;
    };

    template<int GRAPH, typename IN, typename OUT>
    class Extractor : public Appendable<GRAPH, IN>, public Snappable<OUT> {};

    template<int GRAPH, typename INOUT>
    class DefaultExtractor : public Extractor<GRAPH, INOUT, INOUT> {
    public:
        const INOUT snap() {
            return it.load();
        }
        void append(const INOUT& val, WorkState<GRAPH>& ws) {
            if(val != it.exchange(val))
                ws.add_to_queue(*work);
        }
    private:
        std::atomic<INOUT> it;
        std::shared_ptr<Evaluation<GRAPH, INOUT>> work;
        friend class Graph<GRAPH>;
        DefaultExtractor(std::shared_ptr<Evaluation<GRAPH, INOUT>> work) :
            work(work) {}
    };

    template<int GRAPH>
    class Work {
    public:
        const uint32_t id;
        virtual ~Work() {}
    protected:
        virtual void eval(WorkState<GRAPH>&) = 0;
        std::atomic<Work<GRAPH>*> next;
        Work(uint32_t id, Work<GRAPH>* next = nullptr) : 
            id(id), 
            next(next) {}
        Work(const Work&) = delete;
        Work& operator=(const Work&) = delete;
        friend class Graph<GRAPH>;
        friend class WorkState<GRAPH>;
    };

    template<int GRAPH, typename OUT>
    class Evaluation : public Work<GRAPH> {
    public:
        void connect(std::shared_ptr<Appendable<GRAPH, OUT>> a) {
            // spin-lock until success
            while (true) {
                Work<GRAPH>* n = this->next.exchange(this);
                if (n == this) {
                    // it's already locked
                    std::this_thread::yield();
                    continue;
                }
                nexts.push_front(a);
                this->next.store(n); // ...and unlock
                return;
            }
        }
    private:
        std::forward_list<std::shared_ptr<Appendable<GRAPH, OUT>>> nexts;
    protected:
        virtual OUT eval() = 0;
        void eval(WorkState<GRAPH>& ws) {
            // acquire the calculation lock
            auto current = this->next.exchange(this);
            if (current == this) {
                // another calculation in progress
                ws.add_to_queue(*this);
                return;
            }

            OUT val = eval();
            for (auto next : nexts) {
                next->append(val, ws);
            }

            // release the calculation lock, if no-one's
            // re-scheduled us
            this->next.compare_exchange_strong(current, nullptr);
        }
        Evaluation(uint32_t id, Work<GRAPH>* next = nullptr) : Work<GRAPH>(id, next) {}
    };

    template<int GRAPH, typename INPUT>
    class Input final : public Evaluation<GRAPH, INPUT> {
    public:
        void append(INPUT v) {
            this->value.store(v);
            graph->add_to_queue(*this);
        }
    protected:
        INPUT eval() {
            return this->value.load();
        }
    private:
        std::atomic<INPUT> value;
        Graph<GRAPH>& graph;
        Input(Graph<GRAPH>& graph, uint32_t id, INPUT initial) : 
            Evaluation<GRAPH, INPUT>(id, nullptr), 
            graph(graph),
            value(initial) {}
        friend class Graph<GRAPH>;
    };

    template<int GRAPH, typename FN, typename RET, typename... INPUTS>
    class Node final : public Evaluation<GRAPH, RET> {
    protected:
        RET eval() {
            return call_fn(std::index_sequence_for<INPUTS...>{});
        }
    private:
        FN fn;
        std::tuple<INPUTS...> inputs;
        Node(uint32_t id, FN fn, std::tuple<INPUTS...> inputs) :
            Evaluation<GRAPH, RET>(id), fn(fn), inputs(inputs) {}
        friend class Graph<GRAPH>;

        template<std::size_t ...I>
        RET call_fn(std::index_sequence<I...>) {
            return fn(std::get<I>(inputs)->snap()...);
        }
    };

    template<int GRAPH>
    class Graph final : public Work<GRAPH> {
    public:
        Graph() : Work<GRAPH>(0, this), ids(1) {}

        void eval() {
            auto head = this->next.exchange(this);
            if (head == this)
                return;

            auto work = WorkState<GRAPH>(*this);
            for (auto w = head; w != this; w = w->next.load()) {
                work.q.push(w);
            }

            while (!work.q.empty()) {
                work.eval();
            }
        }

        template<typename INPUT>
        std::shared_ptr<Input<GRAPH, INPUT>> input(INPUT initial) {
            return std::shared_ptr<Input<GRAPH, INPUT>>(
                new Input<GRAPH, INPUT>(
                    *this,
                    ids.fetch_add(1),
                    initial));
        }

        template<typename FN, typename... INPUTS>
        auto node(
            const FN fn,
            INPUTS... args) {
            auto ret = connected(fn, connect(args)...);
            add_to_queue(*ret);
            return ret;
        }
    private:
        std::atomic<uint32_t> ids;
        friend class WorkState<GRAPH>;

        void add_to_queue(Work<GRAPH>& w) {
            Work<GRAPH>* snap;
            while (true) {

                // only queue us up if we're unlocked or locked, i.e.
                // not if we're already on the work queue.
                auto lock = w.next.load();
                if (lock != nullptr && lock != &w)
                    return;

                // add w to the queue by chaning its `next` pointer to point
                // to the head of the queue
                snap = this->next.load();
                if (!w.next.compare_exchange_weak(lock, snap))
                    continue;

                if (this->next.compare_exchange_weak(snap, &w))
                    return;

                // if we're here we pointed `w.next` to the head of the queue,
                // but something changed the queue before we could update it
                // to `w`. Undo (i.e. unlock) `w`:
                w.next.store(lock);
            }
        }

        template<typename FN, typename... INPUTS>
        auto connected(
            const FN fn,
            INPUTS... args) -> std::shared_ptr<Evaluation<GRAPH, decltype(fn(args->snap()...))>> {
            using RET = decltype(fn(args->snap()...));
            return std::shared_ptr<Evaluation<GRAPH, RET>>(
                new Node<GRAPH, FN, RET, INPUTS...>(
                    ids.fetch_add(1),
                    fn,
                    std::make_tuple(args...)));
        }

        template<typename INPUT, typename OUTPUT>
        std::shared_ptr<Snappable<OUTPUT>> connect(
            std::shared_ptr<Extractor<GRAPH, INPUT, OUTPUT>> e) {
            return std::static_pointer_cast<
                Snappable<OUTPUT>,
                Extractor<GRAPH, INPUT, OUTPUT>>(e);
        }

        template<typename INOUT>
        std::shared_ptr<Snappable<INOUT>> connect(
            std::shared_ptr<Evaluation<GRAPH, INOUT>> e) {
            auto ret = std::shared_ptr<Extractor<GRAPH, INOUT, INOUT>>(
                    new DefaultExtractor<GRAPH, INOUT>(e));
            e->connect(ret);
            return connect(ret);
        }

        template<typename INOUT>
        std::shared_ptr<Snappable<INOUT>> connect(
            std::shared_ptr<Input<GRAPH, INOUT>> e) {
            return connect(std::static_pointer_cast<
                Evaluation<GRAPH, INOUT>,
                Input<GRAPH, INOUT>>(e));
        }
    protected:
        void eval(WorkState<GRAPH>&) {
            std::abort();
        }
    }; 
}

