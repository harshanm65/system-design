#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <algorithm>

/**
 * A minimal 3‑layer neural network (input → hidden → output) implemented
 * from scratch in C++.  The network uses a sigmoid activation function,
 * mean‑squared error loss, and vanilla stochastic gradient descent.
 *
 * This file demonstrates the essential steps:
 *   1. Weight & bias initialization
 *   2. Forward pass
 *   3. Back‑propagation (computing gradients)
 *   4. Parameter update
 *
 * It is intentionally lightweight so that you can read and modify the
 * mathematics without any external dependencies.
 */

using namespace std;

// Sigmoid activation and its derivative
inline double sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }
inline double sigmoid_derivative(double y) { return y * (1.0 - y); }

// Utility to generate random numbers in [-range, range]
inline double rand_uniform(double range = 1.0) {
    static thread_local mt19937 rng{random_device{}()};
    uniform_real_distribution<double> dist(-range, range);
    return dist(rng);
}

// Simple matrix type: vector of rows
using Matrix = vector<vector<double>>;

// Forward declaration for convenience
struct NeuralNet;

/**
 * Helper: matrix‑vector multiplication
 */
inline vector<double> mat_vec_mul(const Matrix& A, const vector<double>& x) {
    size_t rows = A.size(), cols = A[0].size();
    vector<double> result(rows, 0.0);
    for (size_t i = 0; i < rows; ++i)
        for (size_t j = 0; j < cols; ++j)
            result[i] += A[i][j] * x[j];
    return result;
}

/**
 * Helper: outer product of two vectors
 */
inline Matrix outer_product(const vector<double>& a, const vector<double>& b) {
    size_t rows = a.size(), cols = b.size();
    Matrix result(rows, vector<double>(cols));
    for (size_t i = 0; i < rows; ++i)
        for (size_t j = 0; j < cols; ++j)
            result[i][j] = a[i] * b[j];
    return result;
}

/**
 * Simple 3‑layer feed‑forward network
 *
 * architecture: input_dim → hidden_dim → output_dim
 */
struct NeuralNet {
    size_t input_dim, hidden_dim, output_dim;
    Matrix W1;            // weights between input and hidden
    vector<double> b1;    // bias for hidden layer
    Matrix W2;            // weights between hidden and output
    vector<double> b2;    // bias for output layer

    NeuralNet(size_t in, size_t hid, size_t out)
        : input_dim(in), hidden_dim(hid), output_dim(out),
          W1(hid, vector<double>(in)), b1(hid),
          W2(out, vector<double>(hid)), b2(out) {
        // Randomly initialize weights & biases
        for (auto& row : W1)
            for (double& w : row) w = rand_uniform(0.5);
        fill(b1.begin(), b1.end(), 0.0);

        for (auto& row : W2)
            for (double& w : row) w = rand_uniform(0.5);
        fill(b2.begin(), b2.end(), 0.0);
    }

    /**
     * Forward pass
     *
     * @param x Input vector (size input_dim)
     * @return output vector (size output_dim)
     */
    vector<double> forward(const vector<double>& x,
                           vector<double>& hidden_out) const {
        // Hidden layer pre‑activation
        hidden_out = mat_vec_mul(W1, x);
        for (size_t i = 0; i < hidden_dim; ++i)
            hidden_out[i] += b1[i];

        // Apply sigmoid
        for (auto& v : hidden_out) v = sigmoid(v);

        // Output layer pre‑activation
        vector<double> out = mat_vec_mul(W2, hidden_out);
        for (size_t i = 0; i < output_dim; ++i)
            out[i] += b2[i];

        // Sigmoid again (useful for binary classification)
        for (auto& v : out) v = sigmoid(v);

        return out;
    }

    /**
     * Back‑propagation for a single training example.
     *
     * @param x Input vector
     * @param y Desired output vector
     * @param lr Learning rate
     */
    void train_step(const vector<double>& x,
                    const vector<double>& y,
                    double lr) {
        // Forward pass
        vector<double> hidden_out;
        vector<double> out = forward(x, hidden_out);

        // Compute output error (dL/dout)
        vector<double> delta_out(output_dim);
        for (size_t i = 0; i < output_dim; ++i)
            delta_out[i] = (out[i] - y[i]) * sigmoid_derivative(out[i]);

        // Gradients for W2 and b2
        Matrix dW2 = outer_product(delta_out, hidden_out); // (output_dim x hidden_dim)
        vector<double> db2 = delta_out;                    // (output_dim)

        // Back‑propagate to hidden layer
        vector<double> delta_hidden(hidden_dim, 0.0);
        for (size_t i = 0; i < hidden_dim; ++i) {
            double sum = 0.0;
            for (size_t j = 0; j < output_dim; ++j)
                sum += W2[j][i] * delta_out[j];
            delta_hidden[i] = sum * sigmoid_derivative(hidden_out[i]);
        }

        // Gradients for W1 and b1
        Matrix dW1 = outer_product(delta_hidden, x); // (hidden_dim x input_dim)
        vector<double> db1 = delta_hidden;           // (hidden_dim)

        // Parameter update
        for (size_t i = 0; i < hidden_dim; ++i) {
            b1[i] -= lr * db1[i];
            for (size_t j = 0; j < input_dim; ++j)
                W1[i][j] -= lr * dW1[i][j];
        }

        for (size_t i = 0; i < output_dim; ++i) {
            b2[i] -= lr * db2[i];
            for (size_t j = 0; j < hidden_dim; ++j)
                W2[i][j] -= lr * dW2[i][j];
        }
    }

    /**
     * Simple mean‑squared error over a dataset
     */
    double mse(const vector<vector<double>>& X,
               const vector<vector<double>>& Y) {
        double total = 0.0;
        for (size_t i = 0; i < X.size(); ++i) {
            vector<double> hidden;
            vector<double> out = forward(X[i], hidden);
            for (size_t j = 0; j < output_dim; ++j)
                total += pow(out[j] - Y[i][j], 2);
        }
        return total / X.size();
    }
};

/**
 * Demonstration: XOR problem
 *
 * The XOR function is not linearly separable, so a 2‑hidden neuron network
 * can solve it with sigmoid activations.
 */
int main() {
    // XOR dataset (2 inputs, 1 output)
    vector<vector<double>> X = { {0, 0}, {0, 1}, {1, 0}, {1, 1} };
    vector<vector<double>> Y = { {0}, {1}, {1}, {0} };

    NeuralNet net(2, 2, 1);
    const double lr = 0.5;
    const int epochs = 10000;

    for (int epoch = 1; epoch <= epochs; ++epoch) {
        // Shuffle data each epoch
        vector<size_t> idx(X.size());
        iota(idx.begin(), idx.end(), 0);
        shuffle(idx.begin(), idx.end(), mt19937{random_device{}()});

        for (size_t i : idx)
            net.train_step(X[i], Y[i], lr);

        if (epoch % 1000 == 0) {
            double loss = net.mse(X, Y);
            cout << "Epoch " << epoch << "  MSE: " << loss << '\n';
        }
    }

    // Test the trained network
    cout << "\n--- XOR results after training ---\n";
    for (size_t i = 0; i < X.size(); ++i) {
        vector<double> hidden;
        double out = net.forward(X[i], hidden)[0];
        cout << "Input: (" << X[i][0] << ", " << X[i][1]
             << ") -> Output: " << out
             << "  (rounded: " << round(out) << ")\n";
    }

    return 0;
}
