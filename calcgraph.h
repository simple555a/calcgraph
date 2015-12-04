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

namespace calcgraph {
    class Graph;
    class Work;
    template <typename>
    class Input;
    template <template <typename> class, typename, typename...>
    class Node;
    template <typename>
    class Connectable;
    template <typename>
    class Constant;
    template <template <typename> class, class...>
    class NodeBuilder;

    /**
     * @brief A less-than comparison of Work objects based on their ids
     */
    struct WorkQueueCmp {
        constexpr bool operator()(const Work *a, const Work *b) const;
    };

    template <typename VAL>
    class Storeable {
      public:
        virtual void store(VAL v) = 0;
    };

    /**
     * @brief An input policy that returns the latest value of the Input to the
     * Node to use when its eval() method is called.
     * @details This object is embedded in the Node class, and atomically stores
     * the current value of the input.
     *
     * @tparam VAL The type of the value stored - must be supported by
     * std::atomic.
     */
    template <typename VAL>
    class Latest final : public Storeable<VAL> {
      public:
        using input_type = VAL;
        using output_type = VAL;

        /**
         * @brief Atomically write the latest value into this class
         * @details Used by Upstream dependencies (e.g. those the Node is
         * connected to via the Connectable concept) to pass on new values for
         * the containing Node to evalute when its eval() method is called.
         */
        inline void store(input_type v) override {
            val.store(v, std::memory_order_release);
        }

        /**
         * @brief Used by the node to extract the stored value when its eval()
         * method is called.
         */
        inline output_type read() {
            return val.load(std::memory_order_consume);
        }

        /**
         * @brief Atomically exchange the stored value
         * @details Used by the OnChange propagation policy.
         */
        inline input_type exchange(input_type other) {
            return val.exchange(other, std::memory_order_acq_rel);
        }

        Latest() noexcept : val() {}
        Latest(const Latest &other) noexcept : val(other.val) {}
        Latest(Latest &&other) noexcept : val(std::move(other.val)) {}

      private:
        std::atomic<VAL> val;
    };

    /**
     * @brief A partial specialization of Latest for std::shared_ptr
     * @tparam VAL the type of the value the std::shared_ptr points to
     */
    template <typename VAL>
    class Latest<std::shared_ptr<VAL>> final
        : public Storeable<std::shared_ptr<VAL>> {
      public:
        using input_type = std::shared_ptr<VAL>;
        using output_type = std::shared_ptr<VAL>;

        inline void store(input_type v) override {
            std::atomic_store_explicit(&val, v, std::memory_order_release);
        }
        inline output_type read() {
            return std::atomic_load_explicit(&val, std::memory_order_acquire);
        }
        inline input_type exchange(input_type other) {
            return std::atomic_exchange_explicit(&val, other,
                                                 std::memory_order_acq_rel);
        }

        Latest() noexcept : val() {}
        Latest(const Latest &other) noexcept : val(other.val) {}
        Latest(Latest &&other) noexcept : val(std::move(other.val)) {}

      private:
        std::shared_ptr<VAL> val;
    };

    /**
     * @brief An Input policy that accumulates any values fed to it and returns
     * them all to its containing Node as a std::forward_list
     * @details Useful if you can't afford to drop any values from upstream
     * dependencies
     *
     * @tparam VAL The type of the value stored - doesn't have to be atomic
     */
    template <typename VAL>
    class Accumulate final : public Storeable<VAL> {
      public:
        using input_type = VAL;
        using output_type = std::shared_ptr<std::forward_list<VAL>>;

        /**
         * @brief Atomically adds another value to the ones we've accumulated so
         * far
         */
        inline void store(input_type val) override {
            Element *e = new Element(std::move(val));
            while (true) {
                Element *snap = head.load(std::memory_order_acquire);
                e->next.store(snap, std::memory_order_release);
                if (head.compare_exchange_weak(snap, e))
                    return;
            }
        }

        /**
         * @brief Used by the node to extract any stored values its eval()
         * method is called.
         * @returns An empty list if there have been no new values since the
         * last time read() was called, or a std::forward_list of values in the
         * order that they were received.
         */
        inline output_type read() {
            auto ret = output_type(new std::forward_list<VAL>());

            Element *e = head.exchange(nullptr, std::memory_order_acq_rel);
            while (e != nullptr) {
                ret->push_front(e->val);
                Element *next = e->next.load(std::memory_order_consume);
                delete e;
                e = next;
            }

            return ret;
        }

        Accumulate() noexcept : head() {}
        Accumulate(const Accumulate &other) noexcept : head(other.head) {}
        Accumulate(Accumulate &&other) noexcept : head(std::move(other.head)) {}

      private:
        struct Element final {
            std::atomic<Element *> next;
            VAL val;
            Element(VAL &&val) : next(), val(val) {}
        };

        std::atomic<Element *> head;
    };

    /**
     * @brief A propagation policy that always recalculates downstream
     * dependencies
     * @tparam RET The type of the values calcuated by the Node this policy
     * applies to
     */
    template <typename RET>
    struct Always {
        inline constexpr bool operator()(RET) { return true; }
    };

    /**
     * @brief A propagation policy that recalculates downstream dependencies
     * only if the Node's output changes (according to the != operator)
     * @details This method isn't thread-safe, but doesn't need to be as it's
     * only called by Node.eval() when the calculation lock is held. This policy
     * stores the last output value in the Node, so increases the size of the
     * templated Node class and potentially uses even more memory (e.g. if the
     * output is a std::shared_ptr to a large object).
     * @tparam RET The type of the values calcuated by the Node this policy
     * applies to
     */
    template <typename RET>
    struct OnChange {
        inline bool operator()(RET latest) {
            return last.exchange(latest) != latest;
        }

      private:
        Latest<RET> last;
    };

    /**
     * @brief The state used for a single evalution of a Graph.
     */
    class WorkState final {
      public:
        /**
         * @brief add the given work item to the heap (to be processed later
         * this evalution) or the Graph's work_queue (to be processed next
         * evalution)
         *
         * @todo if we decide to push the Work onto the heap, we could "steal"
         * it from the Graph's work queue if it's on there too, so we don't
         * needlessly evaluate it a second time.
         */
        void add_to_queue(Work &work);

      private:
        /**
         * @brief the Work items to process this evaluation
         *
         * @todo use a fixed-size heap, and push any overflowing Work items back
         * on the Graph's work queue to evaluate later.
         */
        std::priority_queue<Work *, std::vector<Work *>, WorkQueueCmp> q;
        Graph &g;
        struct Stats *stats;
        friend class Graph;
        /**
         * @brief The Work.id of the Work item we're currently processing
         */
        uint32_t current_id;
        WorkState(Graph &g, struct Stats *stats) : g(g), stats(stats) {}
    };

    /**
     * @brief A concept for "something you can connect an Input to"
     * @tparam RET The type of the values consumed (and so the type of values
     * the Input should provide)
     */
    template <typename RET>
    class Connectable {
      public:
        /**
         * @brief Connect the given Input to this object.
         * @details Must be thread-safe and can be called multiple times on any
         * given object.
         */
        virtual void connect(Input<RET>) = 0;

        /**
         * @brief Disconnects the given Input from this object.
         * @details Must be thread-safe and has no effect if the given Input
         * wasn't already connected
         */
        virtual void disconnect(Input<RET>) = 0;
    };

    /**
     * @brief A helper for NodeBuilder.connect, to indicate that the given Input
     * shouldn't be connected to anything.
     * @return An appropriately-cast nullptr that can be passed to connect or
     * NodeBuilder.connect
     */
    template <typename RET>
    inline Connectable<RET> *unconnected() {
        return static_cast<Connectable<RET> *>(nullptr);
    }

    /**
     * @brief Connects the Input to the target Connectable
     * @param to The (possibly-nullptr) Connectable to add this input to
     * @param from A non-nullptr Input to connect
     * @tparam RET the type of the Input and target
     */
    template <typename RET>
    inline void connect(Connectable<RET> *to, Input<RET> from) {
        if (to)
            to->connect(from);
    }

    inline void intrusive_ptr_add_ref(Work *);
    inline void intrusive_ptr_release(Work *);

    namespace flags {
        /**
         * @brief The lock bit in a Work's next pointer.
         */
        static const std::uintptr_t LOCK = 1;
    }

    /**
     * @brief A building block of the graph; either a raw input or code to be
     * evaluated.
     */
    class Work {
      public:
        /**
         * @brief The node's unique id
         * @details Uniqueness is per Graph, and this id is set by the Graph
         * object that created the Work.
         */
        const uint32_t id;

        virtual ~Work() {}

        /**
         * @brief Add this Work to the given Graph's work_queue
         * @details Returns when this Work is added to the work_queue of the
         * given Graph. Could return instantly if already scheduled.
         */
        void schedule(Graph &g);

        /**
         * @brief Actually do the work.
         */
        virtual void eval(WorkState &) = 0;

      protected:
        Work(uint32_t id) : id(id), refcount(0), next(0) {}
        Work(const Work &) = delete;
        Work &operator=(const Work &) = delete;
        friend class WorkState;

      private:
        // for boost's intrinsic_ptr
        std::atomic_uint_fast16_t refcount;
        friend void intrusive_ptr_add_ref(Work *);
        friend void intrusive_ptr_release(Work *);

        /**
         * @page worklocking Locking a Work object
         *
         * We'll use the next pointer of a Work object to store two orthogonal
         * pieces of information. The LSB will store the "locked" - or exclusive
         * lock - flag, and the remaining bits will store the next link in the
         * (intrusive) Graph.work_queue, or nullptr if this node isn't
         * scheduled. Note that the locked flag refers to the Work that contains
         * the next pointer, not the Work pointed to.
         */
      private:
        /**
         * @brief an intrinsic work queue for graph evaluation, headed by
         * Graph.work_queue
         */
        std::atomic<std::uintptr_t> next;

      public:
        /**
         * @brief Fetch and clear the Work item's next-pointer
         * @details This isn't the same as remove this Work item from the
         * Graph's work queue (if it was on it) as it doesn't clear the
         * intrinsic next pointer of any Work item pointing to this one.
         * @returns Extracts the pointer to the next node on the work_queue, or
         * nullptr if this Work isn't on a Graph's work_queue.
         */
        Work *dequeue() {
            std::uintptr_t p =
                next.fetch_and(flags::LOCK, std::memory_order_acq_rel);
            return reinterpret_cast<Work *>(p & ~flags::LOCK);
        }

      protected:
        /**
         * @brief Tries to acquire the Work's exclusive lock
         * @details Not re-entrant
         * @return true if the lock was already taken, false if the lock was
         * successfully acquired.
         */
        bool trylock() {
            bool waslocked =
                next.fetch_or(flags::LOCK, std::memory_order_acq_rel) &
                flags::LOCK;
            return !waslocked;
        }

        /**
         * @brief Release the Work's exclusive lock
         * @details ...by setting the LSB of the next pointer to zero. Only call
         * if you already hold the lock, or the results are undefined.
         */
        void release() {
            next.fetch_and(~flags::LOCK, std::memory_order_release);
        }
    };

    inline void intrusive_ptr_add_ref(Work *w) { ++w->refcount; }

    inline void intrusive_ptr_release(Work *w) {
        if (--w->refcount == 0u) {
            delete w;
        }
    }

    /**
     * @brief Statistics for a single evaluation of the calculation graph.
     */
    struct Stats final {
        /** @brief how many items were taken off the work queue */
        uint16_t queued;
        /** @brief how many Nodes were eval()'ed */
        uint16_t worked;
        /**
         * @brief how many Nodes were added to this evaluation's heap multiple
         * times (as they were dependent on more than one queued or dependent
         * Node)
         */
        uint16_t duplicates;
        /**
         * @brief how many dependencies were pushed back on to the Graph's
         * work_queue to be evaluted next time
         */
        uint16_t pushed_graph;
        /**
         * @brief how many dependencies were pushed onto this evaluation's work
         * heap to be evaluated in topological order
         */
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

    /**
     * @brief A helper member to zero out a Stats object
     */
    static const struct Stats EmptyStats {};

    /**
     * @brief The calcuation-graph-wide state
     * @details This class is the only way to make calculation nodes in the
     * graph, and is in charge of the work_queue, a intrinsic singly-linked list
     * of Work that needs re-evaluating due to upstream changes.
     */
    class Graph final {
      public:
        Graph() : ids(1), tombstone(), work_queue(&tombstone) {}

        /**
         * @brief Run the graph evaluation to evalute all Work items on the
         * work_queue, and all items recursively dependent on them (at least,
         * as determined by each Node's propagation policy).
         * @details Doesn't release the locks we have on the Work items in the
         * queue, as we'll just put them in a heap.
         *
         * @return true iff any Work items were eval'ed
         */
        bool operator()(struct Stats * = nullptr);

        /**
         * @brief Creates a builder object for Nodes
         * @details Sets the default propagation policy as Always.
         */
        NodeBuilder<Always> node();

      private:
        /**
         * @brief The source of Work ids
         */
        std::atomic<uint32_t> ids;

        /**
         * @brief The head of the work queue.
         */
        std::atomic<Work *> work_queue;

        class Tombstone : public Work {
          public:
            void eval(WorkState &) { std::abort(); }

          private:
            Tombstone() : Work(0) {}
            friend class Graph;
        };

        /**
         * @brief The tail of the work queue.
         */
        Tombstone tombstone;

        friend class WorkState;
        template <typename>
        friend class Input;
        template <template <typename> class, class...>
        friend class NodeBuilder;
        friend class Work;

        template <typename... INPUTS, std::size_t... I>
        void connectall(std::index_sequence<I...>,
                        std::tuple<Connectable<INPUTS> *...> tos,
                        std::tuple<Input<INPUTS>...> froms) {
            int forceexpansion[] = {
                0, (connect(std::get<I>(tos), std::get<I>(froms)), 0)...};
        }
    };

    /**
     * @brief An input to a calculation Node
     * @details Used for putting external data into the calcuation graph, and
     * connecting calculation nodes to each other.
     *
     * @tparam INPUT the type of the values this input accepts.
     */
    template <typename INPUT>
    class Input final {
      public:
        /**
         * @brief Sets the input to an externally-provided value, and schedules
         * the Node we're an Input of on the Graph's work_queue for
         * re-evaluation.
         *
         * @param graph The owner of the work queue to add the Input's Node to
         * @param v The new value to set the Input to
         */
        void append(Graph &graph, INPUT v) {
            in->store(v);
            if (ref) {
                ref->schedule(graph);
            }
        }

        Input(Storeable<INPUT> &in) noexcept : in(&in) {}
        Input(const Input &other) noexcept : in(other.in), ref(other.ref) {}
        Input(Input &&other) noexcept : in(std::move(other.in)),
                                        ref(std::move(other.ref)) {}

        /**
         * @brief Equality semantics, based on what it's an Input to, but not
         * what the current value is.
         */
        inline bool operator==(const Input &other) const {
            return in == other.in;
        }
        inline bool operator!=(const Input &other) const {
            return !(*this == other);
        }

      private:
        Storeable<INPUT> *in;

        /**
         * @brief A ref-counted pointer to the owner of the 'in' Latest, so it
         * doesn't get freed while we're still alive
         */
        boost::intrusive_ptr<Work> ref;

        Input(Storeable<INPUT> &in, boost::intrusive_ptr<Work> ref)
            : in(&in), ref(ref) {}

        template <template <typename> class, typename, typename...>
        friend class Node;
        template <typename>
        friend class Accumulator;
        template <template <typename> class, typename>
        friend class MultiConnectable;
        template <typename>
        friend class Constant;
    };

    /**
     * @brief A Connectable object that just passes on its initial value to any
     * Input connected to it
     * @tparam RET The type of the constant value, and so what type of Inputs
     * can be connected
     */
    template <typename RET>
    class Constant : public Connectable<RET> {
      public:
        /**
         * @brief Set the value of this Input to the constant
         * @details The constant is passed on to the Input immediately - not as
         * part of a Graph evaluation.
         */
        void connect(Input<RET> in) override { in.in->store(value); }

        /**
         * @brief Has no effect
         */
        void disconnect(Input<RET> in) override {}

        Constant(RET value) noexcept : value(value) {}
        Constant(Constant &&other) noexcept : value(std::move(other.value)) {}
        Constant(const Constant &other) noexcept : value(other.value) {}

      private:
        RET value;
    };

    /**
     * @brief A Work item that evaluates a function, and propages the results to
     * any connected Inputs.
     * @details The key part of the calculation graph - constructed using a
     * NodeBuilder obtained from Graph.node().
     *
     * @tparam PROPAGATE The propagation policy (e.g. Always or OnChange), used
     * to decide whether to notify connected Inputs after the FN function is
     * evaluated.
     * @tparam FN The function to evaluate. Must be able to operate on
     * default-constructed INPUTS arguments, as it may be invoked before
     * upstream dependencies pass on their values
     * @tparam INPUTS The input policies for the parameters of the FN fuction.
     * You can get the input type of the parameter using INPUTS::type.
     */
    template <template <typename> class PROPAGATE, typename FN, class... INPUTS>
    class Node final
        : public Work,
          public Connectable<
              std::result_of_t<FN(typename INPUTS::output_type...)>> {
      public:
        using RET =
            typename std::result_of_t<FN(typename INPUTS::output_type...)>;

        /**
         * @brief Get an Input object associated with the N'th argument of this
         * Node's function FN
         * @details Pass this object to a Connectable object so this Node is
         * evaluated whenever that Connectable changes, or set values on the
         * Input (i.e. pass values to the FN function) directly using
         * Input.append().
         *
         * @tparam N which function argument to get
         */
        template <std::size_t N>
        auto input() -> Input<std::tuple_element_t<
            N, std::tuple<typename INPUTS::input_type...>>> {
            return Input<std::tuple_element_t<
                N, std::tuple<typename INPUTS::input_type...>>>(
                std::get<N>(inputs), boost::intrusive_ptr<Work>(this));
        }

        /**
         * @brief Return a Tuple of all the Inputs to this function.
         * @details Equivalent to calling input() for the N arguments of the FN
         * function
         */
        std::tuple<Input<typename INPUTS::input_type>...> inputtuple() {
            return inputtuple_fn(std::index_sequence_for<INPUTS...>{});
        }

        /**
         * @brief Connect an Input to the output of this Node's FN function
         * @details Newly-calculated values will be fed to the Input according
         * to this Node's PROPAGATE propagation policy
         */
        void connect(Input<RET> a) override {
            // spinlock until we can add this
            while (!trylock()) {
                std::this_thread::yield();
            }
            dependents.push_front(a);
            release();
        }

        /**
         * @brief Call the FN function on the current INPUTS values, and
         * propagate the result to any connected Inputs
         * @details Propagation is controlled by the PROPAGATE propagation
         * policy. This function exclusively locks the Node so only one thread
         * can call this method at once. If a thread tries to call eval() when
         * the Node is locked, it will instead put this Node back on the
         * WorkState Graph's work_queue.
         */
        void eval(WorkState &ws) override {
            if (!this->trylock()) {
                // another calculation in progress, so put us on the work queue
                // (which will change the next pointer to the next node in the
                // work queue, not this)
                ws.add_to_queue(*this);
                return;
            }

            // there's a race condition here: this Node can be put on the work
            // queue, so if the inputs change we'll get rescheduled and re-ran.
            // We only snap the atomic values in the next statement, so we could
            // pick up newer values than the ones that triggered the
            // recalculation of this Node, so the subsequent re-run would
            // be unnecessary. See the OnChange propagation policy to mitagate
            // this (your function should be idempotent!).

            RET val = call_fn(std::index_sequence_for<INPUTS...>{});

            if (propagate(val)) {
                for (auto dependent = this->dependents.begin();
                     dependent != this->dependents.end(); dependent++) {

                    // pass on the new value & schedule the dowstream work
                    dependent->in->store(val);
                    if (dependent->ref) {
                        ws.add_to_queue(*dependent->ref);
                    }
                }
            }

            this->release();
        }

        /**
         * @brief Disconnect an Input from the output of this Node's FN function
         * @details Has no effect if the given Input wasn't connected in the
         * first place.
         */
        void disconnect(Input<RET> a) override {
            // spinlock until we can add this
            while (!trylock()) {
                std::this_thread::yield();
            }
            dependents.remove(a);
            release();
        }

      private:
        /**
         * @brief Downstream dependencies
         * @details Not threadsafe so controlled by the Work.next LSB locking
         * mechanism
         */
        std::forward_list<Input<RET>> dependents;

        /**
         * @brief The policy on when to propagate newly-calcuated values to
         * dependents
         */
        PROPAGATE<RET> propagate;

        const FN fn;
        std::tuple<INPUTS...> inputs;

        Node(uint32_t id, const FN fn) : Work(id), fn(fn) {}
        friend class Graph;

        template <std::size_t... I>
        inline RET call_fn(std::index_sequence<I...>) {
            return fn(std::get<I>(inputs).read()...);
        }
        template <std::size_t... I>
        inline auto inputtuple_fn(std::index_sequence<I...>) {
            return std::make_tuple<Input<typename INPUTS::input_type>...>(
                input<I>()...);
        }

        template <template <typename> class, class...>
        friend class NodeBuilder;
    };

    /**
     * @brief A builder-pattern object for constructing Nodes
     * @details Can be reused to create multiple Nodes. Arguments can be passed
     * either to successive calls to latest and accumulate (if you want to
     * specify the input policy) or passed all at once to connect (if you just
     * need the default input policy, Latest), or a mixture (in which case the
     * parameters passed to connect come after those passed to latest and
     * accumulate).
     *
     * @tparam PROPAGATE the propagation policy used by the Nodes it constructs
     * @tparam INPUTS the input policies of the first N arguments of the
     *function passed to connect
     */
    template <template <typename> class PROPAGATE, class... INPUTS>
    class NodeBuilder final {
      public:
        /**
         * @brief Change the propagation policy for the Nodes the builder
         * constructs
         * @tparam NEWPROPAGATE the new propagation policy to use
         * @return A new NodeBuilder object
         */
        template <template <typename> class NEWPROPAGATE>
        auto propagate() {
            return NodeBuilder<NEWPROPAGATE, INPUTS...>(g, connected);
        }

        /**
         * @brief Add an argument with an Accumulate input policy
         */
        template <typename VAL>
        auto accumulate(Connectable<VAL> *arg) {
            return doconnect<Accumulate, VAL>(arg);
        }

        /**
         * @brief Add an argument with a Latest input policy
         */
        template <typename VAL>
        auto latest(Connectable<VAL> *arg) {
            return doconnect<Latest, VAL>(arg);
        }

        /**
         * @brief Build a Node
         *
         * @param fn The function the node should execute. The newly-constructed
         * node will be schedule for evaluation on the Graph, so this function
         * should be able to accept default-constructed values of its Inputs (as
         * it may be evaluated before upstream dependencies pass on values to
         * it)
         * @param args Any additional Connectable objects (including
         * calcgraph::unconnected() objects) or appropriately-cast nullptrs to
         * provide values for the calculation function. The Graph will
         * recalcuate the Node when any one of these Connectable objects pass on
         * a new value to the Node (coalescing where possible). This arguments
         * are appended to those passed to this NodeBuilder via calls to
         * accumulate or latest.
         * @tparam VALS The types of the function's arguments, not the input
         * policy.
         * @return A new Node
         */
        template <typename FN, typename... VALS>
        auto connect(const FN fn, Connectable<VALS> *... args) {

            // first, make the node
            auto node = boost::intrusive_ptr<
                Node<PROPAGATE, FN, INPUTS..., Latest<VALS>...>>(
                new Node<PROPAGATE, FN, INPUTS..., Latest<VALS>...>(g.ids++,
                                                                    fn));

            // next, connect any given inputs
            auto newargs =
                std::make_tuple<Connectable<VALS> *...>(std::move(args)...);
            auto concatted = std::tuple_cat(connected, std::move(newargs));
            g.connectall(std::index_sequence_for<INPUTS..., VALS...>{},
                         concatted, node->inputtuple());

            // finally schedule it for evaluation
            node->schedule(g);
            return node;
        }

      private:
        using STORED =
            std::tuple<Connectable<typename INPUTS::input_type> *...>;

        Graph &g;
        STORED
        connected;

        NodeBuilder(Graph &g, STORED connected = STORED())
            : g(g), connected(connected) {}

        friend class Graph;
        template <template <typename> class, class...>
        friend class NodeBuilder;

        template <template <class> class POLICY, typename VAL>
        inline auto doconnect(Connectable<VAL> *arg) {
            auto newarg = std::make_tuple<Connectable<VAL> *>(std::move(arg));
            auto concatted = std::tuple_cat(connected, std::move(newarg));
            return NodeBuilder<PROPAGATE, INPUTS..., POLICY<VAL>>(g, concatted);
        }
    };

    // see comments in declaration
    NodeBuilder<Always> Graph::node() { return NodeBuilder<Always>(*this); }

    // see comments in declaration
    void WorkState::add_to_queue(Work &work) {
        // note that the or-equals part of the check is important; if we failed
        // to calculate work this time then work.id == current_id, and we
        // want to put the work back on the graph queue for later evaluation.
        if (work.id <= current_id) {
            // process it next Graph()
            work.schedule(g);

            if (stats)
                stats->pushed_graph++;
        } else {
            // keep anything around that's going on the heap - we remove a
            // reference after popping them off the heap and eval()'ing them
            intrusive_ptr_add_ref(&work);

            q.push(&work);

            if (stats)
                stats->pushed_heap++;
        }
    }

    constexpr bool WorkQueueCmp::operator()(const Work *a,
                                            const Work *b) const {
        return a->id > b->id;
    }

    // see comments in declaration
    bool Graph::operator()(struct Stats *stats) {
        if (stats)
            *stats = EmptyStats;

        auto head = work_queue.exchange(&tombstone, std::memory_order_acq_rel);
        if (head == &tombstone)
            return false;

        WorkState work(*this, stats);
        Work *w = head;
        while (w != &tombstone) {
            // remove us from the work queue. Note that this is slightly
            // inefficient, as w could be put back on the Graph's work_queue
            // before it's been evaluated in this function call, and so is
            // needlessly evaluated a second time.
            Work *next = w->dequeue();

            work.q.push(w);
            if (stats)
                stats->queued++;

            w = next;
        }

        while (!work.q.empty()) {

            Work *w = work.q.top();
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

            // finally finished with this Work - it's not on the Graph queue or
            // the heap
            intrusive_ptr_release(w);
        }

        return true;
    }

    // see comments in declaration
    void Work::schedule(Graph &g) {

        // don't want work to be deleted while queued
        intrusive_ptr_add_ref(this);

        bool first_time = true;
        while (true) {
            std::uintptr_t current = next.load(std::memory_order_acquire);
            bool locked = current & flags::LOCK;

            if (first_time && (current & ~flags::LOCK)) {
                // we're already on the work queue, as we're pointing to a
                // non-zero pointer
                intrusive_ptr_release(this);
                return;
            }

            // add w to the queue by chaning its next pointer to point
            // to the head of the queue
            Work *head = g.work_queue.load(std::memory_order_acquire);
            if (!next.compare_exchange_weak(
                    current, reinterpret_cast<std::uintptr_t>(head) | locked)) {
                // next was updated under us, retry from the start
                continue;
            }

            if (g.work_queue.compare_exchange_weak(head, this)) {
                // success! but keep the intrustive reference active
                return;
            }

            // if we're here we pointed w.next to the head of the queue,
            // but something changed the queue before we could finish. The
            // next time round the loop we know current will not be nullptr,
            // so set a flag to skip the are-we-already-queued check.
            first_time = false;
        }
    }

    /**
     * @brief Repeatedly evaluate the Graph's work queue
     * @details Evaluate in a busy-loop, only yielding when there's no work to
     * do. Doesn't block. This avoids having to pay the cost of mutex-related
     * system calls if we slept on a mutex (and appending an Input would have to
     * lock the same mutex when notifying this evalution thread).
     *
     * @param g The graph containing the work_queue to evaluate
     * @param stop When set, the thread will exit its busy-loop the next time it
     * sees the work_queue empty
     */
    void evaluate_repeatedly(Graph &g, std::atomic<bool> &stop) {
        while (!stop.load(std::memory_order_consume)) {
            while (g()) {
            }
            std::this_thread::yield();
        }
    }
}

#endif