# CalcGraph {#mainpage}

A lightweight C++14 header-only framework for organising application logic to minimise end-to-end calculation latency. This is designed for many frequently-updated inputs and allows you to trade off processing each input value against ensuring the application logic output reflects the most recent inputs.

This has several advantages:
- Application logic must be stateless (and so easy to unit-test), and any state maintained between invocations is explicitly managed.
- Parallelism for free: the calculation graph is built with lock-free primitives, so many threads can propagate changes through the graph without blocking or data races, and each node in the graph is guaranteed to only be executed by a single thread at once.
- Compile-time verification that the application logic is connected correctly, and all parameters to a piece of logic have been connected or explicitly ignored.

### Logic Evaluation

## Example: Bond Stat-Arb Trading Bot

The `example.cpp` file in the repo is the skeleton of a bond [stat-arb](https://en.wikipedia.org/wiki/Statistical_arbitrage) trading bot. It listens for bond yield quotes for the maturities of a single [issuer](http://www.investopedia.com/terms/i/issuer.asp) sent in UDP datagrams (one quote per datagram, as a text string like `10Y 2.15`), then [bootstraps](https://en.wikipedia.org/wiki/Bootstrapping_%28finance%29) a yield curve from the prices of a fixed set of "benchmark" maturities. The bot then uses the yield curve to generate trading signals, and manages orders based on those signals, tracking the P&L of the trades it makes. As this is just a short example, the yield curve it fits is just a simple two-degree polynomial through the benchmark yields, and the bot just prints the orders it want to execute to its standard out.

This example is a good use-case for the framework as:
- Trading signals should only be based on the most up-to-date market data possible.
- Yield curve bootstrapping is computationally expensive, so we should avoid doing it unless we get new quotes for the benchmark maturities.
- The available maturities aren't available up-front, yet we want to trade all maturities so need to dynamically generate parts of the logic graph.
- We do need to process the contents of all the UDP datagrams, even if we coalesce market data updates (i.e. only use the latest price for a maturity) for most of the processing.

```
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

### Input Policies

### Propagation Policies

### Output Policies

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
