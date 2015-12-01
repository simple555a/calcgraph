#ifndef CALC_H
#define CALC_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <forward_list>
#include <memory>
#include <queue>
#include <thread>
#include <tuple>
#include <sstream>
#include <string>

#include <boost/intrusive_ptr.hpp>

// TODO: fixed size work queue
// TODO: graph loop on change
namespace calc {
    class Graph;
    class Work;
    template<typename>
    class Input;
    template<template<typename> class, typename, typename...>
    class Node;
    template<typename>
    class Connectable;
	template<typename>
	class Constant;
    template<template<typename> class>
    class NodeBuilder;

    struct WorkQueueCmp {
        constexpr bool operator()(const Work* a, const Work* b);
    };

    template<typename VAL>
    class Value final {
    public:
        inline void store(VAL v) {
            val.store(v, std::memory_order_release);
        }
        inline VAL read() {
            return val.load(std::memory_order_acquire);
        }
        inline VAL exchange(VAL other) {
            return val.exchange(other, std::memory_order_acq_rel);
        }

        Value() noexcept : val() {}
        Value(const Value& other) noexcept : val(other.val) {}
        Value(Value&& other) noexcept : val(std::move(other.val)) {}
    private:
        std::atomic<VAL> val;
    };

    template<typename VAL>
    class Value<std::shared_ptr<VAL>> final {
    public:
        inline void store(std::shared_ptr<VAL> v) {
            std::atomic_store_explicit(&val, v, std::memory_order_release);
        }
        inline std::shared_ptr<VAL> read() {
            return std::atomic_load_explicit(&val, std::memory_order_acquire);
        }
        inline std::shared_ptr<VAL> exchange(std::shared_ptr<VAL> other) {
            return std::atomic_exchange_explicit(&val, other, std::memory_order_acq_rel);
        }

        Value() noexcept : val() {}
        Value(const Value& other) noexcept : val(other.val) {}
        Value(Value&& other) noexcept : val(std::move(other.val)) {}
    private:
        std::shared_ptr<VAL> val;
    };

    template<typename RET>
    struct Always {
        inline constexpr bool operator()(RET) {
            return true;
        }
    };

    template<typename RET>
    struct OnChange {
        inline bool operator()(RET latest) {
            return last.exchange(latest) != latest;
        }
    private:
        Value<RET> last;
    };

    class WorkState final {
    public:
        void add_to_queue(Work& work);
    private:
        std::priority_queue<
            Work*,
            std::vector<Work*>,
            WorkQueueCmp> q;
        Graph& g;
        struct Stats* stats;
        friend class Graph;
        uint32_t current_id;
        WorkState(Graph& g, struct Stats* stats) : g(g), stats(stats) {}
    };

    template<typename RET>
    class Connectable {
    public:
    	virtual void connect(Input<RET>) = 0;
    };

    template<typename RET>
    inline Connectable<RET>* unconnected() {
    	return static_cast<Connectable<RET>*>(nullptr);
    }

    template<typename RET>
    inline void connect(Connectable<RET>* to, Input<RET> from) {
    	if (to)
    		to->connect(from);
    }

    inline void intrusive_ptr_add_ref(Work*);
    inline void intrusive_ptr_release(Work*);

    namespace flags {
        static const std::uintptr_t LOCK = 1;
    }

    /**
     * A building block of the graph; either a raw input or code to be evaluated. This class handles the work queue.
     */
    class Work {
    public:
    	// the unique node id
        const uint32_t id;

        virtual ~Work() {}

        /**
         * Returns when this `Work` is added to the `work_queue` of the given `Graph`. Could return instantly if already scheduled.
         */
        void schedule(Graph& g);

        /**
         * Actually do the work.
         */
        virtual void eval(WorkState&) = 0;
    protected:
        Work(uint32_t id) : 
            id(id), 
            refcount(0),
            next(0) {}
        Work(const Work&) = delete;
        Work& operator=(const Work&) = delete;
        friend class WorkState;

    private:
        // for boost's intrinsic_ptr
        std::atomic_uint_fast16_t refcount;
        friend void intrusive_ptr_add_ref(Work*);
        friend void intrusive_ptr_release(Work*);

    /**
     * LOCKING
     * 
     * We'll use the `next` pointer to store two orthogonal pieces of information. The LSB will store the "locked" - or exclusive lock - flag, and the remaining bits will store the next link in the (intrusive) `Graph.work_queue`, or nullptr if this node isn't scheduled. Note that the locked flag refers to the Work that contains the `next` pointer, not the Work pointed to.
     */
    private:
        /*
         * an intrinsic work queue for graph evaluation, headed by `Graph.work_queue`.
         */
        std::atomic<std::uintptr_t> next;

    public:
        /**
         * Extracts the pointer to the next node on the `work_queue`, or nullptr if it's not there.
         */
        Work* readnext() {
            std::uintptr_t p = next.load(std::memory_order_acquire);
            return reinterpret_cast<Work*>(p & ~flags::LOCK);
        }

    protected:
        /**
         * Not re-entrant; tries to acquire the lock and returns true if the lock was already taken.
         */
        bool trylock() {
            return next.fetch_or(flags::LOCK, std::memory_order_acquire) & flags::LOCK;
        }

        /**
         * release the calculation lock by setting the LSB to zero. Only call if you already hold the lick
         */
        void release() {
            next.fetch_and(~flags::LOCK, std::memory_order_release);
        }

        /**
         * As trylock, but will also reset the queue pointer to nullptr, taking us off the queue
         */
         bool trylock_and_dequeue() {
            std::uintptr_t p = next.load(std::memory_order_acquire);
            return !(p & flags::LOCK) && next.compare_exchange_strong(p, flags::LOCK, std::memory_order_acq_rel);
         }
    };


    inline void intrusive_ptr_add_ref(Work* w) {
        ++w->refcount;
    }

    inline void intrusive_ptr_release(Work* w) {
        if (--w->refcount == 0u) {
            delete w;
        }
    }

    /**
     * Statistics for a single evaluation of the calculation graph.
     */
    struct Stats {
        // how many items were taken off the work queue
    	uint16_t queued;
        // how many Nodes were eval()'ed
    	uint16_t worked;
        // how many Nodes were added to this evaluation's heap multiple times (as they were dependent on more than one queued or dependent Node).
    	uint16_t duplicates;
        // how many dependencies were pushed back on to the Graph's `work_queue` to be evaluted next time
        uint16_t pushed_graph;
        // how many dependencies were pushed onto this evaluation's work heap to be evaluated in topological order
        uint16_t pushed_heap;

        operator std::string() const {
            std::ostringstream out;  
            out << "queued: " << queued;
            out << ", worked: " << worked;
            out << ", duplicates: " << duplicates;
            out << ", pushed_graph: " << pushed_graph;
            out << ", pushed_heap: " << pushed_heap;
            return out.str();
        }
    };
    static const struct Stats EmptyStats {};

    class Graph final {
    public:
        Graph() : 
            ids(1),
            tombstone(),
            work_queue(&tombstone) {}

        /**
         * Run the graph evaluation to evalute all Work items on the `work_queue`, and all items recursively dependent on them (at least, as determined by each `Node`'s propagation policy).
         *
         * @return true iff any Work items were eval'ed
         */
        bool operator()(struct Stats* = nullptr);
        
        NodeBuilder<Always> node();

    private:
        std::atomic<uint32_t> ids;

        std::atomic<Work*> work_queue;

        class Tombstone : public Work {
        public:
            void eval(WorkState&) {
                std::abort();
            }
        private:
            Tombstone() : Work(0) {}
            friend class Graph;
        };
        Tombstone tombstone;

        friend class WorkState;
	    template<typename>
	    friend class Input;
        template<template<typename> class>
        friend class NodeBuilder;
        friend class Work;

        template <typename ...INPUTS, std::size_t ...I>
        void connectall(
        	std::index_sequence<I...>,
        	std::tuple<Connectable<INPUTS>*...> tos,
        	std::tuple<Input<INPUTS>...> froms) {
        	int forceexpansion[] = { 0, ( connect(std::get<I>(tos), std::get<I>(froms)), 0) ... };
        }
    };

    template<typename INPUT>
    class Input final {
    public:
	    void append(Graph& graph, INPUT v) {
	        in->store(v);
	        if (ref) {
	        	ref->schedule(graph);
	        }
	    }

        Input(Value<INPUT>& in) noexcept : in(&in) {}
        Input(const Input& other) noexcept : in(other.in), ref(other.ref) {}
        Input(Input&& other) noexcept : in(std::move(other.in)), ref(std::move(other.ref)) {}

    private:
        Value<INPUT>* in;
		
		// so the target doesn't get GC'ed
		boost::intrusive_ptr<Work> ref;

        Input(
        	Value<INPUT>& in,
        	boost::intrusive_ptr<Work> ref) : in(&in), ref(ref) {}

    	template<template<typename> class, typename, typename...>
    	friend class Node;
    	template<typename>
    	friend class Constant;
    };

    template<typename RET>
    class Constant : public Connectable<RET> {
    public:
    	void connect(Input<RET> in) {
    		in.in->store(value);
    	}

        Constant(RET value) noexcept : value(value) {}
        Constant(Constant&& other) noexcept : value(std::move(other.value)) {}
        Constant(const Constant& other) noexcept : value(other.value) {}
    private:
    	RET value;
    };

    template<template<typename> class PROPAGATE, typename FN, typename... INPUTS>
    class Node final : public Work, public Connectable<std::result_of_t<FN(INPUTS...)>> {
    public:
        using RET = typename std::result_of_t<FN(INPUTS...)>;

    	template<std::size_t N>
        auto input() -> Input<std::tuple_element_t<N, std::tuple<INPUTS...>>> {
        	return Input<std::tuple_element_t<N, std::tuple<INPUTS...>>>(
        		std::get<N>(inputs),
        		boost::intrusive_ptr<Work>(this));
        }

        std::tuple<Input<INPUTS>...> inputtuple() {
        	return inputtuple_fn(std::index_sequence_for<INPUTS...>{});
        }

        void connect(Input<RET> a) override {
            // spinlock until we can add this
            while (!trylock()) {
                std::this_thread::yield();
            }
            dependents.push_front(a);
            release();
        }

        void eval(WorkState& ws) override {
            if (!trylock_and_dequeue()) {
                // another calculation in progress, so put us on the work queue
                // (which will change the `next` pointer to the next node in the
                // work queue, not `this`)
                ws.add_to_queue(*this);
                return;
            }

            // there's a race condition here: this Node can be put on the work queue, so
            // if the inputs change we'll get rescheduled and re-ran. We only snap the atomic
            // values in the next statement, so we could pick up newer values than the ones that
            // triggered the recalculation of this Node, so the subsequent re-run would be
            // unnecessary. See the OnChange propagation policy to mitagate this (your function
            // should be idempotent!).

            // calculate ourselves
            RET val = call_fn(std::index_sequence_for<INPUTS...>{});

            if (propagate(val)) {
                for (
                	auto dependent = dependents.begin();
                	dependent != dependents.end();
                	dependent++) {

                	// pass on the new value & schedule the dowstream work
                    dependent->in->store(val);
                	if (dependent->ref) {
                		ws.add_to_queue(*dependent->ref);
                	}
                }
            }

            release();
        }
    private:
        const FN fn;
        std::tuple<Value<INPUTS>...> inputs;

        Node(uint32_t id, const FN fn) : Work(id), fn(fn) {}
        friend class Graph;

        template<std::size_t ...I>
        RET call_fn(std::index_sequence<I...>) {
            return fn(std::get<I>(inputs).read()...);
        }
        template<std::size_t ...I>
        auto inputtuple_fn(std::index_sequence<I...>) {
            return std::make_tuple<Input<INPUTS>...>(input<I>()...);
        }

        template<template<typename> class>
        friend class NodeBuilder;

    /**
     * PROPAGATION
     */
    private:
        // downstream nodes, not threadsafe so controlled by the `Work.next` LSB lock
    	std::forward_list<Input<RET>> dependents;

        // the policy on when to propagate new values to dependents
        PROPAGATE<RET> propagate;
    };

    template<template<typename> class PROPAGATE>
    class NodeBuilder final {
    public:
        template<template<typename> class NEWPROPAGATE>
        NodeBuilder<NEWPROPAGATE> propagate() {
            return NodeBuilder<NEWPROPAGATE>(g);
        }

        template<typename FN, typename... INPUTS>
        auto connect(
            const FN fn,
            Connectable<INPUTS>*... args) {

                // first, make the node
                auto node = boost::intrusive_ptr<Node<PROPAGATE, FN, INPUTS...>>(
                    new Node<PROPAGATE, FN, INPUTS...>(g.ids++, fn));

                // next, connect any given inputs
                g.connectall(
                    std::index_sequence_for<INPUTS...>{},
                    std::make_tuple<Connectable<INPUTS>*...>(std::move(args)...),
                    node->inputtuple());

                // finally schedule it for evaluation
                node->schedule(g);
                return node;
        }
    private:
        Graph& g;
        NodeBuilder(Graph& g) : g(g) {}

        friend class Graph;
        template<template<typename> class>
        friend class NodeBuilder;
    };

    NodeBuilder<Always> Graph::node() {
        return NodeBuilder<Always>(*this);
    }

    /** 
     * 
     */
    void WorkState::add_to_queue(Work& work) {
        // note that the or-equals part of the check is important; if we failed
        // to calculate `work` this time then `work.id` == `current_id`, and we want
        // to put the work back on the graph queue for later evaluation.
        if (work.id <= current_id) {
            // process it next `Graph()`
            work.schedule(g);

            if (stats)
                stats->pushed_graph++;
        } else {
            // keep anything around that's going on the heap - we remove a reference
            // after popping them off the heap and `eval()`'ing them
            intrusive_ptr_add_ref(&work);

            // FIXME
            q.push(&work);

            if (stats)
                stats->pushed_heap++;
        }
    }

    constexpr bool WorkQueueCmp::operator()(const Work* a, const Work* b) {
        return a->id > b->id;
    }

    /**
     * Doesn't release the locks we have on the Work* items in the queue, as we'll
     * just put them in a heap.
     */
    bool Graph::operator()(struct Stats* stats) {
        if (stats)
            *stats = EmptyStats;

        auto head = work_queue.exchange(&tombstone);
        if (head == &tombstone)
            return false;

        auto work = WorkState(*this, stats);
        for (auto w = head; w != &tombstone; w = w->readnext()) {
            work.q.push(w);
            if (stats)
                stats->queued++;
        }

        while (!work.q.empty()) {

            Work* w = work.q.top();
            work.q.pop();

            // remove any duplicates, we only need to
            // calculate things once.
            while (!work.q.empty() && work.q.top()->id == w->id) {
                intrusive_ptr_release(work.q.top());
                work.q.pop();
                if (stats)
                    stats->duplicates++;
            }

            work.current_id = w->id;
            w->eval(work);
            if (stats)
                stats->worked++;

            // finally finished with this Work - it's not on the Graph queue or the heap
            intrusive_ptr_release(w);
        }

        return true;
    }

    void Work::schedule(Graph& g) {

        // don't want work to be deleted while queued
        intrusive_ptr_add_ref(this);

        bool first_time = true;
        while (true) {
            std::uintptr_t current = next.load(std::memory_order_acquire);
            bool locked = current & flags::LOCK;

            if (first_time && (current & ~flags::LOCK)) {
                // we're already on the work queue, as we're pointing to a non-zero
                // pointer
                intrusive_ptr_release(this);
                return;
            }

            // add w to the queue by chaning its `next` pointer to point
            // to the head of the queue
            Work* head = g.work_queue.load(std::memory_order_acquire);
            if (!next.compare_exchange_weak(current, reinterpret_cast<std::uintptr_t>(head) | locked)) {
                // next was updated under us, retry from the start
                continue;
            }

            if (g.work_queue.compare_exchange_weak(head, this)) {
                // success! but keep the intrustive reference active
                return;
            }

            // if we're here we pointed `w.next` to the head of the queue,
            // but something changed the queue before we could finish. The
            // next time round the loop we know `current` will not be nullptr,
            // so set a flag to skip the are-we-already-queued check.
            first_time = false;
        }
    }

    void evaluate_repeatedly(Graph& g, std::atomic<bool>& stop) {
        while (!stop.load(std::memory_order_consume)) {
            while (g());
            std::this_thread::yield();
        }
    }
}

#endif