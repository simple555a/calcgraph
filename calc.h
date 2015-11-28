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

#include <boost/intrusive_ptr.hpp>

// TODO: shared pointers
// TODO: load / store mem ordering
namespace calc {
    class Graph;
    class Work;
    template<typename INPUT>
    class Input;
    template<typename FN, typename... INPUTS>
    class Node;
    template<typename RET>
    class Connectable;
	template<typename RET>
	class Constant;
    template<template<typename> class PROPAGATE>
    class NodeBuilder;

    struct WorkQueueCmp {
        constexpr bool operator()(const Work* a, const Work* b);
    };

    template<typename RET>
    struct Always {
        bool operator()(RET) {
            return true;
        }
    };

    template<typename RET>
    struct OnChange {
        bool operator()(RET latest) {
            return last.exchange(latest) != latest;
        }
    private:
        std::atomic<RET> last;
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
        friend class Graph;
        uint32_t current_id;
        WorkState(Graph& g) : g(g) {}
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

    /**
     * A building block of the graph; either a raw input or code to be evaluated. This class handles the work queue.
     */
    class Work {
    public:
    	// the unique node id
        const uint32_t id;

        virtual ~Work() {}
    protected:

        virtual void eval(WorkState&) = 0;

        /*
         * an intrinsic work queue for graph evaluation.
         */
        std::atomic<Work*> next;

        Work(uint32_t id, Work* next = nullptr) : 
            id(id), 
            next(next),
            refcount(0) {}
        Work(const Work&) = delete;
        Work& operator=(const Work&) = delete;
        friend class Graph;
        friend class WorkState;

    private:
        // for boost's intrinsic_ptr
        std::atomic_uint_fast16_t refcount;

        friend void intrusive_ptr_add_ref(Work*);
        friend void intrusive_ptr_release(Work*);
    };


    inline void intrusive_ptr_add_ref(Work* w) {
        ++w->refcount;
    }

    inline void intrusive_ptr_release(Work* w) {
        if (--w->refcount == 0u) {
            delete w;
        }
    }

    struct Stats {
    	uint16_t queued;
    	uint16_t worked;
    	uint16_t duplicates;
    };
    static const struct Stats EmptyStats {};

    class Graph final : public Work {
    public:
        Graph() : Work(0, this), ids(1) {}

        void operator()(struct Stats* = nullptr);
        
        NodeBuilder<Always> node();

    private:
        std::atomic<uint32_t> ids;

        friend class WorkState;
	    template<typename INPUT>
	    friend class Input;
        template<template<typename> class PROPAGATE>
        friend class NodeBuilder;

        void add_to_queue(Work& w);

        template <typename ...INPUTS, std::size_t ...I>
        void connectall(
        	std::index_sequence<I...>,
        	std::tuple<Connectable<INPUTS>*...> tos,
        	std::tuple<Input<INPUTS>...> froms) {
        	int forceexpansion[] = { 0, ( connect(std::get<I>(tos), std::get<I>(froms)), 0) ... };
        }

    protected:
        void eval(WorkState&) {
            std::abort();
        }
    };

    template<typename INPUT>
    class Input final {
    public:
	    void append(Graph& graph, INPUT v) {
	        in->store(v);
	        if (ref) {
	        	graph.add_to_queue(*ref);
	        }
	    }

        Input(std::atomic<INPUT>& in) noexcept : in(&in) {}
        Input(const Input& other) noexcept : in(other.in), ref(other.ref) {}
        Input(Input&& other) noexcept : in(std::move(other.in)), ref(std::move(other.ref)) {}

    private:
        std::atomic<INPUT>* in;
		
		// so the target doesn't get GC'ed
		boost::intrusive_ptr<Work> ref;

        Input(
        	std::atomic<INPUT>& in,
        	boost::intrusive_ptr<Work> ref) : in(&in), ref(ref) {}

    	template<typename FN, typename... INPUTS>
    	friend class Node;
    	template<typename RET>
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

    template<typename FN, typename... INPUTS>
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
            auto n = spinlock();
            dependents.push_front(a);
            next.store(n); // ...and unlock
        }

    protected:
        void eval(WorkState& ws) override {
            auto current = trylock();

            if (current == this) {
                // another calculation in progress
                ws.add_to_queue(*this);
                return;
            }

            // calculate ourselves
            RET val = call_fn(std::index_sequence_for<INPUTS...>{});

            for (
            	auto dependent = dependents.begin();
            	dependent != dependents.end();
            	dependent++) {

            	// pass on the new value & schedule the dowstream work
            	// TODO: pluggable propagation
                dependent->in->store(val);
            	if (dependent->ref) {
            		ws.add_to_queue(*dependent->ref);
            	}
            }

            release();
        }
    private:
        const FN fn;
        std::tuple<std::atomic<INPUTS>...> inputs;

        Node(uint32_t id, const FN fn) : Work(id), fn(fn) {}
        friend class Graph;

        template<std::size_t ...I>
        RET call_fn(std::index_sequence<I...>) {
            return fn(std::get<I>(inputs).load()...);
        }
        template<std::size_t ...I>
        auto inputtuple_fn(std::index_sequence<I...>) {
            return std::make_tuple<Input<INPUTS>...>(input<I>()...);
        }

        template<template<typename> class PROPAGATE>
        friend class NodeBuilder;
    /**
     * Downstream nodes
     */
    private:
    	std::forward_list<Input<RET>> dependents;

    /**
     * Uses the 'next' pointer as a lock. Returns 'this' if already locked, or
     * the old value if the lock was successful.
     */
    private:
        Work* trylock() {
        	return next.exchange(this);
        }
        Work* spinlock() {
            while (true) {
                auto n = trylock();
                if (n != this) {
                	return n;
                }

                // it's already locked
                std::this_thread::yield();
            }
        }

        /**
         * release the calculation lock, if no-one's re-scheduled us
         */
        void release() {
        	Work* t = this;
        	next.compare_exchange_strong(t, nullptr);
        }
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
                auto node = boost::intrusive_ptr<Node<FN, INPUTS...>>(new Node<FN, INPUTS...>(g.ids++, fn));

                // next, connect any given inputs
                g.connectall(
                    std::index_sequence_for<INPUTS...>{},
                    std::make_tuple<Connectable<INPUTS>*...>(std::move(args)...),
                    node->inputtuple());

                // finally schedule it for evaluation
                g.add_to_queue(*node);
                return node;
        }
    private:
        Graph& g;
        NodeBuilder(Graph& g) : g(g) {}
        friend class Graph;
    };

    NodeBuilder<Always> Graph::node() {
        return NodeBuilder<Always>(*this);
    }

    /** 
     * TODO: fixed size work queue
     */
    void WorkState::add_to_queue(Work& work) {
        if (work.id <= current_id)
            // process it next Graph#eval
            g.add_to_queue(work);
        else {
            // spin-lock it and add to our queue
            intrusive_ptr_add_ref(&work);
            q.push(&work);
        }
    }

    constexpr bool WorkQueueCmp::operator()(const Work* a, const Work* b) {
        return a->id > b->id;
    }

    /**
     * Doesn't release the locks we have on the Work* items in the queue, as we'll
     * just put them in a heap.
     *
     * TODO: fixed size work queue
     */
    void Graph::operator()(struct Stats* stats) {
        if (stats)
            *stats = EmptyStats;

        auto head = this->next.exchange(this);
        if (head == this)
            return;

        auto work = WorkState(*this);
        for (auto w = head; w != this; w = w->next.load()) {
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
    }

    void Graph::add_to_queue(Work& w) {
        // don't want work to be deleted while queued
        intrusive_ptr_add_ref(&w);

        while (true) {

            // only queue us up if we're unlocked or locked, i.e.
            // not if we're already on the work queue.
            auto lock = w.next.load();
            if (lock != nullptr && lock != &w) {
                intrusive_ptr_release(&w);
                return;
            }

            // add w to the queue by chaning its `next` pointer to point
            // to the head of the queue
            Work* snap = this->next.load();
            if (!w.next.compare_exchange_weak(lock, snap))
                continue;

            if (this->next.compare_exchange_weak(snap, &w))
                // success! but keep the intrustive reference active
                return;

            // if we're here we pointed `w.next` to the head of the queue,
            // but something changed the queue before we could update it
            // to `w`. Undo (i.e. unlock) `w`:
            w.next.store(lock);
        }
    }
}

#endif