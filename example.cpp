#include <thread>
#include <gsl/gsl_multifit.h>
#include <cmath>

#include "calcgraph.h"

const std::vector<double> dx = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
const std::vector<double> dy = {1, 6, 17, 34, 57, 86, 121, 162, 209, 262, 321};

const double degree = 3;

int main() {
    double chisq;
    auto X = gsl_matrix_alloc(dx.size(), degree);
    auto y = gsl_vector_alloc(dx.size());
    auto c = gsl_vector_alloc(degree);
    auto cov = gsl_matrix_alloc(degree, degree);

    for (int i = 0; i < dx.size(); i++) {
        for (int j = 0; j < degree; j++) {
            gsl_matrix_set(X, i, j, pow(dx[i], j));
        }
        gsl_vector_set(y, i, dy[i]);
    }

    auto ws = gsl_multifit_linear_alloc(dx.size(), degree);
    gsl_multifit_linear(X, y, c, cov, &chisq, ws);

    std::vector<double> out = std::vector<double>(degree);
    for (int i = 0; i < degree; i++) {
        out.push_back(gsl_vector_get(c, i));
    }

    gsl_multifit_linear_free(ws);
    gsl_matrix_free(X);
    gsl_matrix_free(cov);
    gsl_vector_free(y);
    gsl_vector_free(c);
    return 0;
}