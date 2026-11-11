#ifndef MACHOMAN_COCOA_LAYOUT_H
#define MACHOMAN_COCOA_LAYOUT_H
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
namespace ak::layout {
// Top-left coordinates. Index -1 refers to the fixed containing view.
struct frame {
    double x, y, w, h;
    double width_stay = 1, height_stay = 1;
};
struct constraint {
    int first, attribute, second = -1, second_attribute = 0, relation = 0;
    double multiplier = 1, constant = 0, priority = 1000;
};
inline void resize_axis(double &origin, double &length, double old_size, double delta, unsigned mask) {
    const std::array<double, 3> spans{std::max(0., origin), std::max(0., length),
                                      std::max(0., old_size - origin - length)};
    double total = 0;
    int count = 0;
    for (int i = 0; i < 3; ++i)
        if (mask & (1u << i)) {
            total += spans[i];
            ++count;
        }
    if (!count)
        return;
    if (mask & 1)
        origin += delta * (total > 0 ? spans[0] / total : 1. / count);
    if (mask & 2)
        length = std::max(0., length + delta * (total > 0 ? spans[1] / total : 1. / count));
}
inline std::array<double, 4> coefficients(int attribute) {
    switch (attribute) {
    case 1:
    case 5:
        return {1, 0, 0, 0}; // left, leading
    case 2:
    case 6:
        return {1, 0, 1, 0}; // right, trailing
    case 3:
        return {0, 1, 0, 0}; // top
    case 4:
        return {0, 1, 0, 1}; // bottom
    case 7:
        return {0, 0, 1, 0};
    case 8:
        return {0, 0, 0, 1};
    case 9:
        return {1, 0, .5, 0};
    case 10:
        return {0, 1, 0, .5};
    default:
        return {}; // Unsupported baseline/layout guide attributes.
    }
}
inline void solve(std::vector<frame> &frames, double width, double height, const std::vector<constraint> &constraints) {
    const int n = int(frames.size()) * 4;
    if (!n || constraints.empty())
        return;
    std::vector<double> initial(n), result(n);
    for (size_t i = 0; i < frames.size(); ++i) {
        auto f = frames[i];
        initial[i * 4] = f.x;
        initial[i * 4 + 1] = f.y;
        initial[i * 4 + 2] = f.w;
        initial[i * 4 + 3] = f.h;
    }
    result = initial;
    struct equation {
        std::vector<double> a;
        double b, weight;
        int relation;
    };
    std::vector<equation> equations;
    for (auto c : constraints) {
        if (c.first < -1 || c.first >= int(frames.size()) || c.second < -1 || c.second >= int(frames.size()))
            continue;
        if (c.attribute < 1 || c.attribute > 10 || c.second_attribute < 0 || c.second_attribute > 10)
            continue;
        equation e{std::vector<double>(n), c.constant, std::pow(10., std::clamp(c.priority / 250., 0., 4.)),
                   c.relation};
        auto add = [&](int index, int attr, double factor) {
            auto a = coefficients(attr);
            if (index == -1)
                e.b -= factor * (a[2] * width + a[3] * height);
            else
                for (int k = 0; k < 4; ++k)
                    e.a[index * 4 + k] += factor * a[k];
        };
        add(c.first, c.attribute, 1);
        if (c.second_attribute)
            add(c.second, c.second_attribute, -c.multiplier);
        equations.push_back(std::move(e));
    }
    // A weak stay on positions and a stronger intrinsic-size stay select a
    // stable solution for underconstrained views. Inequalities activate only
    // when violated; priority controls how conflicting constraints resolve.
    std::vector<bool> active(equations.size());
    for (int pass = 0; pass < 12; ++pass) {
        std::vector<std::vector<double>> m(n, std::vector<double>(n + 1));
        for (int i = 0; i < n; ++i) {
            double stay = i % 4 < 2 ? .0001 : i % 4 == 2 ? frames[i / 4].width_stay : frames[i / 4].height_stay;
            m[i][i] = stay;
            m[i][n] = initial[i] * stay;
        }
        for (std::size_t k = 0; k < equations.size(); ++k) {
            auto &e = equations[k];
            double current = 0;
            for (int i = 0; i < n; ++i)
                current += e.a[i] * result[i];
            if (!active[k] && e.relation < 0 && current <= e.b + .00001)
                continue;
            if (!active[k] && e.relation > 0 && current >= e.b - .00001)
                continue;
            active[k] = true;
            for (int i = 0; i < n; ++i)
                if (e.a[i]) {
                    m[i][n] += e.a[i] * e.b * e.weight;
                    for (int j = 0; j < n; ++j)
                        m[i][j] += e.a[i] * e.a[j] * e.weight;
                }
        }
        for (int col = 0; col < n; ++col) {
            int pivot = col;
            for (int row = col + 1; row < n; ++row)
                if (std::abs(m[row][col]) > std::abs(m[pivot][col]))
                    pivot = row;
            std::swap(m[col], m[pivot]);
            if (std::abs(m[col][col]) < 1e-12)
                continue;
            for (int row = col + 1; row < n; ++row) {
                double factor = m[row][col] / m[col][col];
                for (int j = col; j <= n; ++j)
                    m[row][j] -= factor * m[col][j];
            }
        }
        auto previous = result;
        for (int row = n - 1; row >= 0; --row) {
            double value = m[row][n];
            for (int j = row + 1; j < n; ++j)
                value -= m[row][j] * result[j];
            if (std::abs(m[row][row]) > 1e-12)
                result[row] = value / m[row][row];
        }
        double change = 0;
        for (int i = 0; i < n; ++i)
            change = std::max(change, std::abs(result[i] - previous[i]));
        if (change < .0001)
            break;
    }
    for (size_t i = 0; i < frames.size(); ++i)
        if (std::isfinite(result[i * 4]) && std::isfinite(result[i * 4 + 1]) && std::isfinite(result[i * 4 + 2]) &&
            std::isfinite(result[i * 4 + 3]))
            frames[i] = {result[i * 4], result[i * 4 + 1], std::max(0., result[i * 4 + 2]),
                         std::max(0., result[i * 4 + 3])};
}
} // namespace ak::layout
#endif
