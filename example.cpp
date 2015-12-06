#include <thread>
#include <gsl/gsl_multifit.h>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <unordered_map>
#include <set>
#include <string>

#include "calcgraph.h"

using double_vector = std::shared_ptr<std::vector<double>>;
using price_input_map =
    std::unordered_map<std::uint8_t, calcgraph::Input<double>>;
using price_input_map_ptr = std::shared_ptr<price_input_map>;
using uint8_set = std::set<uint8_t>;
using uint8_set_ptr = std::shared_ptr<uint8_set>;
using string = std::shared_ptr<std::string>;
using strings = std::shared_ptr<std::forward_list<string>>;

/**
 * @brief Polyfit quadratic functions
 */
static const uint8_t DEGREE = 3;

/**
 * @brief Port to listen on
 */
static const short PORT = 8080;

/**
 * @brief Global termination flag, so we can set it in signal handlers
 */
static std::atomic<bool> stop(false);

/**
 * @brief Size of the UDP datagram buffer
 */
static const int buffer_len = 4096;

static calcgraph::Constant<uint8_set_ptr>
    BENCHMARKS(uint8_set_ptr(new uint8_set({1, 3, 5, 10})));

static calcgraph::Graph g;

/**
 * @brief The distance from the interpolated yield curve a price must be to
 * trigger a "buy" or "sell" signal
 */
static const double THRESHOLD = 0.01;

enum TradeSignal { BUY, SELL, HOLD };

/**
 * @brief Polynomial curve fitting on 2D data
 * @see http://rosettacode.org/wiki/Polynomial_regression#C
 */
double_vector polyfit(const double_vector dx, const double_vector dy) {
    double chisq;
    auto X = gsl_matrix_alloc(dx->size(), DEGREE);
    auto y = gsl_vector_alloc(dx->size());
    auto c = gsl_vector_alloc(DEGREE);
    auto cov = gsl_matrix_alloc(DEGREE, DEGREE);

    for (int i = 0; i < dx->size(); i++) {
        for (int j = 0; j < DEGREE; j++) {
            gsl_matrix_set(X, i, j, pow(dx->at(i), j));
        }
        gsl_vector_set(y, i, dy->at(i));
    }

    auto ws = gsl_multifit_linear_alloc(dx->size(), DEGREE);
    gsl_multifit_linear(X, y, c, cov, &chisq, ws);

    double_vector out = double_vector(new std::vector<double>(DEGREE));
    for (int i = 0; i < DEGREE; i++) {
        out->push_back(gsl_vector_get(c, i));
    }

    gsl_multifit_linear_free(ws);
    gsl_matrix_free(X);
    gsl_matrix_free(cov);
    gsl_vector_free(y);
    gsl_vector_free(c);
    return out;
}

calcgraph::Input<double> build_pipeline(uint8_t ticker) {
    auto signal_generator =
        g.node()
            .propagate<calcgraph::OnChange>()
            .latest(calcgraph::unconnected<double>())
            .latest(calcgraph::unconnected<double_vector>())
            .connect([ticker](double price,
                              double_vector yield_curve) -> TradeSignal {
                if (!yield_curve || !price) {
                    return HOLD; // not initialized properly
                }

                // work out the model price ("fair value") from our fitted yield
                // curve
                double fair_value = 0.0;
                for (uint8_t i = 0; i < DEGREE; ++i) {
                    fair_value += pow(ticker, i) * yield_curve->at(i);
                }
                printf("price %0.2f vs FV %0.2f for %dY\n", price, fair_value,
                       ticker);

                // if the market price deviates from the model price by more
                // than a THRESHOLD amount, generate a trading signal
                if (price > fair_value + THRESHOLD)
                    return SELL;
                else if (price < fair_value - THRESHOLD)
                    return BUY;
                else
                    return HOLD;
            });

    return signal_generator->input<0>();
}

price_input_map_ptr dispatch(strings msgs, price_input_map_ptr map,
                             uint8_set_ptr benchmarks) {
    if (!map) {
        map = price_input_map_ptr(new price_input_map());
    }

    std::for_each(msgs->begin(), msgs->end(), [map](string &msg) {
        // parse the message
        uint8_t ticker = std::stoi(*msg);
        double price = std::stod(msg->substr(msg->find(" ") + 1));

        // pass it to the right output
        auto input = map->find(ticker);
        if (input == map->end()) {
            input = map->insert({ticker, build_pipeline(ticker)}).first;
        }
        input->second.append(g, price);
    });
    return map;
}

/**
 * @brief Set up a UDP socket and pass any (complete) received datagrams to the
 * Input.
 * @returns true iff the listening process started correctly
 */
bool listen_to_datagrams(calcgraph::Input<string> &&in) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        perror("socket");
        return false;
    }
    int oval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &oval, sizeof(oval)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        return false;
    }

    // set up a timeout so we check the "stop" flag once a second (to break out
    // of the receive loop)
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        return false;
    }
    struct sockaddr_in myaddr = {.sin_family = AF_INET,
                                 .sin_addr = htonl(INADDR_ANY),
                                 .sin_port = htons(PORT)};
    if (bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) {
        perror("bind");
        return false;
    }

    char buffer[buffer_len];
    struct iovec iov = {.iov_base = buffer, .iov_len = buffer_len};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    int byterecv;
    while (!stop.load()) {
        if ((byterecv = recvmsg(fd, &msg, 0)) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == EINPROGRESS) {
                continue; // probably timeout
            } else {
                perror("recvmsg");
                return false;
            }
        } else if (msg.msg_flags & MSG_TRUNC) {
            continue; // skip broken packets
        } else {
            in.append(g, string(new std::string(buffer, byterecv)));
        }
    }
    return true;
}

void install_sigint_handler() {
    signal(SIGINT, [](int signum) {
        stop.store(true);
        std::cerr << "SIGINT, exiting" << std::endl;
    });
}

int main() {
    install_sigint_handler();

    std::thread t(calcgraph::evaluate_repeatedly, std::ref(g), std::ref(stop));

    auto dispatcher = g.node()
                          .propagate<calcgraph::Weak>()
                          .accumulate(calcgraph::unconnected<string>())
                          .latest(calcgraph::unconnected<price_input_map_ptr>())
                          .latest(&BENCHMARKS)
                          .connect(dispatch);
    dispatcher->connect(dispatcher->input<1>());

    if (!listen_to_datagrams(dispatcher->input<0>())) {
        stop.store(true);
    }

    t.join();
    return 0;
}