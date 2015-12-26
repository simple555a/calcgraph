# CalcGraph {#mainpage}

A lightweight C++14 header-only framework for organising application logic to minimise end-to-end calculation latency. This is designed for many frequently-updated inputs and allows you to trade off processing each input value against ensuring the application logic output reflects the most recent inputs.

This has several advantages:
- Application logic must be stateless (and so easy to unit-test), and any state maintained between invocations is explicitly managed.
- Parallelism for free: the calculation graph is built with lock-free primitives, so many threads can propagate changes through the graph without blocking or data races, and each node in the graph is guaranteed to only be executed by a single thread at once.
- Compile-time verification that the application logic is connected correctly, and all parameters to a piece of logic have been connected or explicitly ignored.

### Overview

Application logic is broken down into stateless functions, each of which is embedded in a graph "node". The node's "inputs" are connected to the function's parameters and the node's output is connected to the function's return value. The application logic also connects the nodes together (they form a directed cyclic graph), and is responsible for passing external data to the inputs of the relevant nodes. The graph can then be evaluated; all nodes whose inputs have changed have their functions invoked, and their return values are passed into the inputs of any connected nodes. This process continues recursively until there are no nodes left with unprocessed input values.

## Example: Bond Stat-Arb Trading Bot

The `example.cpp` file in the repo is the skeleton of a bond [stat-arb](https://en.wikipedia.org/wiki/Statistical_arbitrage) trading bot. It listens for bond yield quotes for the maturities of a single [issuer](http://www.investopedia.com/terms/i/issuer.asp) sent in UDP datagrams (one quote per datagram, as a text string like `10Y 2.15`), then [bootstraps](https://en.wikipedia.org/wiki/Bootstrapping_%28finance%29) a yield curve from the prices of a fixed set of "benchmark" maturities. The bot then uses the yield curve to generate trading signals, and manages orders based on those signals, tracking the P&L of the trades it makes. As this is just a short example, the yield curve it fits is just a simple two-degree polynomial through the benchmark yields, and the bot just prints the orders it want to execute to its standard out.

This example is a good use-case for the framework as:
- Trading signals should only be based on the most up-to-date market data possible.
- Yield curve bootstrapping is computationally expensive, so we should avoid doing it unless we get new quotes for the benchmark maturities.
- The available maturities aren't available up-front, yet we want to trade all maturities so need to dynamically generate parts of the logic graph.
- We do need to process the contents of all the UDP datagrams, even if we coalesce market data updates (i.e. only use the latest price for a maturity) for most of the processing.

```c++
enum TradeSignal { BUY, SELL, HOLD };
static const char *TradeSignalNames[] = {"BUY", "SELL", "HOLD"};

/**
 * The "benchmark" maturities, or instruments we'll consider when building (via
 * polyfit) the yield curve.
 */
static std::set<uint8_t> BENCHMARKS = {1, 5, 10};

/**
 * @brief The distance from the interpolated yield curve a price must be to
 * trigger a "buy" or "sell" signal
 */
static const double THRESHOLD = 0.1;

/**
 * @brief An order on an exchange; the position the trading bot wants to take.
 */
struct Order final {
    const uint8_t maturity;
    const TradeSignal type;
    const double price;

    Order(uint8_t maturity, TradeSignal type, double price)
        : maturity(maturity), type(type), price(price) {
        printf("opening %s @ %0.3f on %dY\n", TradeSignalNames[type], price,
               maturity);
    }

    void close(double current_price) {
        printf("closing %s @ %0.3f on %dY at %0.3f, P&L %0.3f\n",
               TradeSignalNames[type], price, maturity, current_price,
               (current_price - price) * (type == BUY ? 1 : -1));
    }
};

using double_vector = std::shared_ptr<std::vector<double>>;
/**
 * @brief Fit a polynomial curve to 2-dimensional data
 */
double_vector polyfit(
                      const std::shared_ptr<std::vector<uint8_t>> dx,
                      const std::shared_ptr<std::vector<double>> dy);

/**
 * @brief Set up a UDP socket and pass any (complete) received datagrams to the
 * given Input
 */
bool listen_to_datagrams(calcgraph::Input<std::shared_ptr<std::string>>> &&in);

using uint8double_vector =
    std::shared_ptr<std::vector<std::pair<uint8_t, double>>>;
/**
 * @brief Parse the given quotes into maturity-yield pairs.
 */
uint8double_vector dispatch(
    std::shared_ptr<std::forward_list<std::shared_ptr<std::string>>> msgs) {
    uint8double_vector ret =
        uint8double_vector(new uint8double_vector::element_type());
    for (auto msg : *msgs) {
        uint8_t maturity = std::stoi(*msg);
        double price = std::stod(msg->substr(msg->find(" ") + 1));
        ret->emplace_back(maturity, price);
    }
    return ret;
}

static calcgraph::Graph g;

int main() {
    // create a single thread to constantly propagate changes through the graph
    std::thread t(calcgraph::evaluate_repeatedly, std::ref(g), std::ref(stop));

    // A node to the graph to parse the quotes in from the UDP datagrams and
    // de-multiplex the parsed yields to different nodes of the calculation
    // graph.
    auto dispatcher =
        g.node()
            // the OnChange propagation policy means this Node will only
            // propagate values to other nodes in the calculation graph if they
            // are different, so if we receive consecutive quotes for the same
            // maturity with the same yield we won't do any work for the
            // duplicates
            .propagate<calcgraph::OnChange>()
            // set the input policy for the UDP datagrams to "Accumulate", so
            // we build up unprocessed strings in a list, and process all the
            // new messages in a single batch when this node is evaluated. This
            // means we can be sure we don't lose any quotes (by accidentally
            // coalescing them).
            .accumulate(calcgraph::unconnected<string>())
            // the MultiValued part of the output policy is because this node
            // processed a batch of messages, but we want to 'destructure' the
            // batch and pass the parsed quotes one-by-one to downstream logic.
            .output<calcgraph::MultiValued<calcgraph::Demultiplexed>::type>()
            .connect(dispatch);

    // This node runs the yield-curve bootstrapping algorithm, returning the
    // polynomial curve's coefficients as a vector of doubles
    auto curve_fitter = g.node()
                            .propagate<calcgraph::OnChange>()
                            // the curve fitting function `polyfit` is fairly
                            // generic, and just takes list of maturities and
                            // yields. We'll make these "variadic", which
                            // allows us to connect individual maturity and
                            // double inputs to this Node, and the framework
                            // will invoke the `polyfit` method by taking the
                            // latest value from each connected Input and
                            // putting them in a std::vector.
                            .variadic<uint8_t>()
                            .variadic<double>()
                            .connect(polyfit);

    for (uint8_t benchmark : BENCHMARKS) {
        // for each benchmark maturity (in the statically-defined set in this
        // example, but could be a list from a database or configuration file),
        // connect the yields for the maturity that come out of the dispatcher
        // node to the curve fitter node, so the yield curve is re-bootstrapped
        // if a new quote arrives
        curve_fitter->variadic_add<0>(benchmark);
        auto price = dispatcher->keyed_output(benchmark);
        price.connect(curve_fitter->variadic_add<1>(NAN));

        // Call the function below to build out the part of the calculation
        // graph that generates trading signals and then trades for this
        // benchmark maturity.
        build_pipeline(benchmark, price, curve_fitter.get(), NAN);
    }

    // Embed some logic into the dispatcher node that has access to the
    // instantiated output policy, to extend the calculation graph to trade any
    // new maturities we see.
    dispatcher->embed(
        [&curve_fitter](auto new_pair, auto &output) {
            // get the output of the dispatcher node for the new maturity (the
            // first member of the pair; the second is the parsed yield from
            // the quote that triggered this logic)
            auto price = output.keyed_output(new_pair->first);
            // Call the same function below to build out the part of the
            // calculation graph that generates trading signals and then trades
            // for the new maturity.
            build_pipeline(new_pair->first, *price, curve_fitter.get(),
                           new_pair->second);
        });

    // the main thread will block listening for UDP datagrams, feeding any new
    // ones it receives into the dispatcher's input
    listen_to_datagrams(dispatcher->input<0>());

    t.join();
    return 0;
}

/**
 * @brief Build and connect application logic to generate trading signals and
 * then orders for the given maturity
 */
void build_pipeline(uint8_t maturity, calcgraph::Connectable<double> &price,
                    calcgraph::Connectable<double_vector> *curve,
                    double initial_price) {
    auto signal_generator =
        g.node()
            // Only re-evaluate the order-management logic below if the trading
            // signal changes
            .propagate<calcgraph::OnChange>()
            // connect the signal generation logic to the latest yield for the
            // maturity, and seed it with initial_value (which may be the yield
            // from the quote of a new maturity that caused us to build this
            // 'pipeline')
            .latest(&price, initial_price)
            // We also only care about the latest fitted yield curve
            .latest(curve)
            .connect([maturity](double price, double_vector yield_curve) {

                if (!yield_curve || std::isnan(price)) {
                    return HOLD; // not enough quotes received yet
                }

                // work out the model price ("fair value") from our fitted yield
                // curve polynomial
                double fair_value = 0.0;
                for (uint8_t i = 0; i < DEGREE; ++i) {
                    fair_value += pow(maturity, i) * yield_curve->at(i);
                }

                // if the market price deviates from the model price by more
                // than a THRESHOLD amount, generate a trading signal
                if (price > fair_value + THRESHOLD)
                    return SELL;
                else if (price < fair_value - THRESHOLD)
                    return BUY;
                else
                    return HOLD;
            });

    // This "order manager" generates orders based on the trading signal. In a
    // real application it'd send changes in its orders to an exchange (or
    // broker), and process the responses (which would probably be an
    // additional Accumulated input). However, this dummy implementation will
    // just print trades using printf statements in the Order struct's
    // constructor & close method.
    // This Node is a good example of how to represent state between evaluation
    // of the order management logic - in this case, the current Order struct
    // that represents the trading bot's position in this maturity.
    auto order_manager =
        g.node()
            // The output of this function is connected back to the third
            // argument of the order management logic lambda, so we want to set
            // a propagation policy that doesn't cause an infinite loop
            // re-evaluating the order management node as it's input keeps
            // changing. The Weak policy pushes the changed output value to
            // downstream nodes (in this case, itself) but doesn't schedule the
            // downstream nodes for evaluation. Instead this node will only be
            // re-evaluated if one (or both) of its other inputs (the current
            // yield of this maturity or the bootstrapped yield curve
            // coefficients) change - yet will still see the correct value of
            // the current position (Order).
            .propagate<calcgraph::Weak>()
            // The order management logic also needs the current yield of this
            // maturity so we know what level to open and close orders at.
            .latest(&price, initial_price)
            .latest(signal_generator.get(), HOLD)
            // The framework makes sure we explicitly mention unconnected
            // inputs so we can be sure we haven't forgotten anything at
            // compile-time. As we're connecting the output of this node to
            // itself, we need to construct it first with an unconnected input,
            // then connect up the input in the next statement.
            .unconnected<std::shared_ptr<Order>>()
            .connect([maturity](double price, TradeSignal sig,
                              std::shared_ptr<Order> current) {

                // our order generation algorithm isn't particularly
                // sophisticated; we'll make a trade as soon as we see a
                // signal, and hold the trade until the signal flips to the
                // opposite recommendation (i.e. we'll hold a BUY order until
                // our signal suggests we enter a SELL order, at which point
                // we'll close the BUY and open a SELL). This means we'll tend
                // to always have an open position in each maturity we're
                // watching.
                switch (sig) {
                case HOLD:
                    return current;
                case BUY:
                case SELL:
                    if (current) {
                        if (current->type == sig) {
                            return current; // keep holding
                        } else {
                            current->close(price);
                        }
                    }
                    return std::shared_ptr<Order>(new Order(maturity, sig, price));
                }
            });

    // and finally connect the output of the order management - the persistent
    // 'state' of this function - back to itself.
    order_manager->connect(order_manager->input<2>());
}
```

## Getting Started

The `Graph` object contains the work queue of nodes that need evaluating due to changes in their inputs. It's also used to construct new nodes in the calculation graph; `Graph::node()` returns you a builder object that allows you to wrap an existing function and customise the input, propagation and output policies (described below).

### Connecting Logic

Blocks of logic are connected via the `Connectable` and `Input` interfaces. All graph nodes constructed using the builder implement `Connectable`, and all nodes have an `Node::input()` method (with the parameter number as a template parameter) that gives you an Input object. You can also push values into a Node directly using the `Input::append` method (which also takes the `Graph` as a parameter to the node that created the Input can be scheduled for re-evaluation). All Node objects are reference-counted (the builder returns a `boost::intrusive_ptr` to a newly-created Node, giving it an initial reference count of one) and `Input` objects hold a (counted) reference to their creating Node, so an Input can never outlive the Node it belongs to. Passing an `Input` to `Connectable::connect` stores the `Input` in the Connectable object, so a Node will never be deleted if it's still connected to anything (and similiarly, a node will be automatically deleted once it's no longer connected to anything, unless you keep additional `boost::intrusive_ptr`s to it).

### Evaluating the Graph's Work Queue

When new values are passed to a graph node via `Input::append`, the Node is scheduled on the graph's work queue. `Graph::operator()()` is a thread-safe method to remove all outstanding work from the queue, and evaluate the "dirty" nodes one by one in the order of their `Work::id` fields. After a dirty node has been evaluated, any connected inputs are always added to the `std::priority_queue` heap of nodes to evaluate (skipping duplicates; i.e. nodes that are already in the heap ready for evaluation), depending on the propagation policy. This node-by-node evaluation continues until the heap is empty, at which point `Graph::operator()()` returns. Now, cycles in the logic graph are expected, so to avoid entering an infinite loop the function only evaluates nodes in strictly monotonically-increasing order. If the next node on the heap has a lower or equal id to the node that was just evaluated, it is removed from the heap and put back on the Graph's work queue.

A helper method `evaluate_repeatedly` repeated calls `Graph::operator()()` on the graph passed as an argument, yielding if the run queue is empty. This method is designed for a dedicated thread to use so it can process graph updates as they come in without blocking, at the cost of fully-utilizing the core the thread is scheduled on.

### Input Policies

Each node in the calculation graph is responsible for storing its own input values. How they're stored, and how the (and which) values are passed to the node's function is determined by the input policy. Each argument to the function has its own independent input policy, and the initial value of the input (that will be passed to the node's function if no other input values have been receieved) is also configurable via the `NodeBuilder` object. The policies include:

- **Latest**, the most frequently-used policy, stores a single parameter value in an atomic variable. New values just replace the existing stored value, so the graph node's function only sees the most up-to-date value (and may not see every value that's ever been fed to the input). The are partial template specializations of the Latest policy to support `std::shared_ptr` and `boost::intrustive_ptr` values if you need to pass objects to the node's function that can't be stored in a `std::atomic` object. The following `NodeBuilder` arguments add a parameter to the builder object with a `Latest` policy:
    - **latest(Connectable*, initial = {})** adds a parameter and connects the parameter of any graph node that the builder creates to the given `Connectable` object. It also sets the parameter's initial value to the supplied value (or a default-constructed value, if not given).
    - **initialize(value)** adds a parameter with the given initial value, but doesn't connect the input to anything
    - **unconnected()** adds a parameter with a default-constructed initial value and doesn't connect the input to anything
- **Accumulate** is a policy that stores every new value in a lock-free single-linked list, and when the node's function is evaluated the current contents of the list is passed to the parameter as a `std::forward_list` args. As this is is thread-safe, the input can be connected to multiple sources, and all collected values are passed in the order they are received. To add a parameter with this policy to a `NodeBuilder` builder object, use the `accumulate(Connectable*)` function (optionally specifying a Connectable to wire the node up to when it's created).
- **Variadic** is for when you want to connect a variable number of inputs to the graph node, but want the values from those inputs to be passed to the node's function as a single `std::vector`. Specifying this policy (via `NodeBuilder::variadic()`) means the created nodes will have `variadic_add` and `variadic_remove` methods, which let you connect and disconnect values from the parameter after the node's constructed.

### Propagation Policies

Once a node's function is calculated, the node's propagation policy is used to determine whether to pass that value on to any inputs, and, if so, whether to schedule those inputs for re-evaluation by adding them to the `Graph`'s work queue. The policy must have two methods:

- **bool push_value(value)**: A Node stores its input values according to its parameters' input policies; this method is used by a node to determine whether to pass the value that it just calculated to the input policies of its connected nodes (the `Input`s that were stored in the node's output policy when they were passed to the node's `Connectable::connect` implementation).
- **bool notify()** If `push_value` returns true for a value it is passed to all the node's connected inputs - then `notify()` is called to determine whether to add those inputs to the graph's work queue for subsequent evaluation.

The propagation policy of a node can be changed using the `NodeBuilder::propagate()` parameterized method (before it's constructed), and available policies are:

- **Always** (the default) just returns true for both methods. It passes all values it sees to connected Inputs without any additional processing.
- **Weak** returns true for `push_value`, so passes every value it sees to the connected Inputs. However, it always returns false for `notify()`, so never schedules downstream nodes for recalcuation.
- **OnChange** is a more complex policy, and is used to coalesce duplicates to reduce the number of times downstream nodes are calculated (by assuming downstream logic is idempotent). It stores the last value the function evaluated to, and if immediately-following values are equal then `push_value` returns false and the duplicates are dropped. There's an partial specialization for `std::shared_ptr` that determines value equality based on the value pointed to (rather than just the `std::shared_ptr` object itself). 

### Output Policies

A node's output policy determines how connected Inputs are stored and how values from the function are passed through to them. They usually embed (one or more copies of) the node's propagation policy to help them make this decision. The policies are:

- **SingleList** (the default) stores the connected Inputs (including duplicates) in a `std::vector`.
- **MultiValued** wraps another output policy (e.g. `MultiValued<SingleList>::type`), and when invoked iterates over the output of the node's function (using `std::begin` and `std::end`), passing each element iterated over to its nested output policy. This is useful when the node operates on a batch of data, but connected nodes only expect to process the data one-by-one.
- **Demultiplexed** is the most complex policy. It works with nodes whose functions output a `std::pair` of values, and treats the first element of each value as a key into the `std::unordered_map` of instances of the SingleList output policy it contains. It passes the second element of the pair to the output policy it finds - or if none exists it passes the whole pair to a separate SingleList policy instance for "unkeyed" items. The Node::connect and Node::disconnect functions delegate through to this "unkeyed" policy. Nodes templated with a Demultiplexed output policy also have a `keyed_output` method that takes a key and returns a Connectable object. The implementation of `keyed_output` looks up the given key in the policy's unordered_map, and if it doesn't find it it creates a new instance of the SingleList policy and adds it to the map. The Connectable object the method returns is connected to this SingleList policy, so passing an Input to its `Connectable::connect` or `Connectable::disconnect` methods adds or removes the Input from the SingleList's `std::vector`. This output policy is conceptually the inverse of a variadic input, and takes care only to schedule downstream nodes connected to keyed inputs (i.e. Inputs stored in the policy's unordered_map) if the node's function outputs a value with that key (so the calculation graph nodes attached to unrelated keys aren't needlessly recalculated).

All data structures in all the output policy implementations are guarded by the containing node's lock, so all modifications of these datastructures spin on the lock until it is free. This is enforced by the Node itself, so the implementations don't contain any locking logic themselves.

## Dependencies

- CalcGraph uses boost intrusive_ptr, a header-only smart pointer library.
- The tests (`make check`) use [cppunit](http://sourceforge.net/projects/cppunit) and [valgrind](http://valgrind.org).
- To build the documentation (`make doc`), you need [doxygen](http://www.stack.nl/~dimitri/doxygen) and [pdflatex](https://www.ctan.org/pkg/pdftex).
- The example (`make example`) uses the [GNU Scientific Library](https://www.gnu.org/software/gsl), and optionally a [ruby](https://www.ruby-lang.org/en) interpreter for the script to drive the example process with dummy data.

## Contributing

Set up the `clang-format` git filter used by the `.gitattributes` to invoke clang-format by running:

```
$ git config filter.clang-format.clean clang-format
$ git config filter.clang-format.smudge cat
```

The canonical version of the code is [hosted on Github](https://github.com/remis-thoughts/calcgraph).
