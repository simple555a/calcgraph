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

namespace calc {
    class Graph;
    class Work;
    template<typename INPUT>
    class Input;
    template<typename FN, typename RET, typename... INPUTS>
    class Node;
    template<typename RET>
    class Connectable;
	template<typename RET>
	class Constant;

    struct WorkQueueCmp {
        constexpr bool operator()(const Work* a, const Work* b);
    };

    class WorkState final {
    public:
        void add_to_queue(Work& work);
        void operator()();
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

    class Graph final : public Work {
    public:
        Graph() : Work(0, this), ids(1) {}

        void operator()();
        
        template<typename FN, typename... INPUTS>
        auto node(
            const FN fn,
            Connectable<INPUTS>*... args);

    private:
        std::atomic<uint32_t> ids;

        friend class WorkState;
	    template<typename INPUT>
	    friend class Input;

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
	        this->in->store(v);
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
        	boost::intrusive_ptr<Work> ref) : in(in), ref(ref) {}

    	template<typename FN, typename RET, typename... INPUTS>
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

    template<typename FN, typename RET, typename... INPUTS>
    class Node final : public Work, public Connectable<RET> {
    public:
    	template<std::size_t N>
        auto input() -> Input<std::tuple_element_t<N, std::tuple<INPUTS...>>> {
        	return Input<std::tuple_element_t<N, std::tuple<INPUTS...>>>(std::get<N>(this->inputs));
        }

        std::tuple<Input<INPUTS>...> inputtuple() {
        	return inputtuple_fn(std::index_sequence_for<INPUTS...>{});
        }

        void connect(Input<RET> a) override {
            auto n = this->spinlock();
            dependents.push_front(a);
            this->next.store(n); // ...and unlock
        }

    protected:
        void eval(WorkState& ws) override {
            auto current = this->trylock();

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

            // release the calculation lock, if no-one's re-scheduled us
            this->next.compare_exchange_strong(current, nullptr);
        }
    private:
        FN fn;
        std::tuple<std::atomic<INPUTS>...> inputs;

        Node(uint32_t id, FN fn) : Work(id), fn(fn) {}
        friend class Graph;

        template<std::size_t ...I>
        RET call_fn(std::index_sequence<I...>) {
            return fn(std::get<I>(inputs).load()...);
        }
        template<std::size_t ...I>
        auto inputtuple_fn(std::index_sequence<I...>) {
            return std::make_tuple<Input<INPUTS>...>(this->input<I>()...);
        }

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
        	return this->next.exchange(this);
        }
        Work* spinlock() {
            while (true) {
                auto n = this->trylock();
                if (n != this) {
                	return n;
                }

                // it's already locked
                std::this_thread::yield();
            }
        }
    };

    template<typename FN, typename... INPUTS>
    auto Graph::node(
        const FN fn,
        Connectable<INPUTS>*... args) {
    	
    	// first, make the node
    	using RET = typename std::result_of<FN(INPUTS...)>::type;
    	auto node = new Node<FN, RET, INPUTS...>(this->ids++, fn);

    	// next, connect any given inputs
    	connectall(
    		std::index_sequence_for<INPUTS...>{},
    		std::make_tuple<Connectable<INPUTS>*...>(std::move(args)...),
    		node->inputtuple());

    	// finally schedule it for evaluation
    	this->add_to_queue(*node);
    	return boost::intrusive_ptr<Node<FN, RET, INPUTS...>>(node);
    }
}

#endif