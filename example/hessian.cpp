// 
// Copyright 2019 Olzhas Zhumabek <anonymous.from.applecity@gmail.com>
// 
// Distributed under the Boost Software License, Version 1.0 
// See accompanying file LICENSE_1_0.txt or copy at 
// http://www.boost.org/LICENSE_1_0.txt 
//

#include <boost/gil/image.hpp>
#include <boost/gil/image_view.hpp>
#include <boost/gil/image_processing/numeric.hpp>
#include <boost/gil/image_processing/hessian.hpp>
#include <boost/gil/extension/io/png.hpp>
#include <vector>
#include <functional>
#include <set>
#include <iostream>
#include <fstream>

namespace gil = boost::gil;

// Demonstrates Hessian feature (blob) detection

// some images might produce artifacts
// when converted to grayscale,
// which was previously observed on
// canny edge detector for test input
// used for this example.
// the algorithm here follows sRGB gamma definition
// taken from here (luminance calculation):
// https://en.wikipedia.org/wiki/Grayscale
gil::gray8_image_t to_grayscale(gil::rgb8_view_t original)
{
    gil::gray8_image_t output_image(original.dimensions());
    auto output = gil::view(output_image);
    constexpr double max_channel_intensity = (std::numeric_limits<std::uint8_t>::max)();
    for (long int y = 0; y < original.height(); ++y)
    {
        for (long int x = 0; x < original.width(); ++x)
        {
            // scale the values into range [0, 1] and calculate linear intensity
            auto& p = original(x, y);
            double red_intensity = p.at(std::integral_constant<int, 0>{})
                / max_channel_intensity;
            double green_intensity = p.at(std::integral_constant<int, 1>{})
                / max_channel_intensity;
            double blue_intensity = p.at(std::integral_constant<int, 2>{})
                / max_channel_intensity;
            auto linear_luminosity = 0.2126 * red_intensity
                                    + 0.7152 * green_intensity
                                    + 0.0722 * blue_intensity;

            // perform gamma adjustment
            double gamma_compressed_luminosity = 0;
            if (linear_luminosity < 0.0031308)
            {
                gamma_compressed_luminosity = linear_luminosity * 12.92;
            } else
            {
                gamma_compressed_luminosity = 1.055 * std::pow(linear_luminosity, 1 / 2.4) - 0.055;
            }

            // since now it is scaled, descale it back
            output(x, y) = gamma_compressed_luminosity * max_channel_intensity;
        }
    }

    return output_image;
}

void apply_gaussian_blur(gil::gray8_view_t input_view, gil::gray8_view_t output_view)
{
    constexpr static std::ptrdiff_t filter_height = 5ull;
    constexpr static std::ptrdiff_t filter_width = 5ull;
    constexpr static double filter[filter_height][filter_width] =
    {
        { 2,  4,  6,  4,  2 },
        { 4, 9, 12, 9,  4 },
        { 5, 12, 15, 12,  5 },
        { 4, 9, 12, 9,  4 },
        { 2,  4,  5,  4,  2 }
    };
    constexpr double factor = 1.0 / 159;
    constexpr double bias = 0.0;

    const auto height = input_view.height();
    const auto width = input_view.width();
    for (std::ptrdiff_t x = 0; x < width; ++x)
    {
        for (std::ptrdiff_t y = 0; y < height; ++y)
        {
            double intensity = 0.0;
            for (std::ptrdiff_t filter_y = 0; filter_y < filter_height; ++filter_y)
            {
                for (std::ptrdiff_t filter_x = 0; filter_x < filter_width; ++filter_x)
                {
                    int image_x = x - filter_width / 2 + filter_x;
                    int image_y = y - filter_height / 2 + filter_y;
                    if (image_x >= input_view.width() || image_x < 0 ||
                        image_y >= input_view.height() || image_y < 0)
                    {
                        continue;
                    }
                    const auto& pixel = input_view(image_x, image_y);
                    intensity += pixel.at(std::integral_constant<int, 0>{})
                        * filter[filter_y][filter_x];
                }
            }
            auto& pixel = output_view(gil::point_t(x, y));
            pixel = (std::min)((std::max)(int(factor * intensity + bias), 0), 255);
        }

    }
}

std::vector<gil::point_t> suppress(
    gil::gray32f_view_t harris_response,
    double harris_response_threshold)
{
    std::vector<gil::point_t> corner_points;
    for (gil::gray32f_view_t::coord_t y = 1; y < harris_response.height() - 1; ++y)
    {
        for (gil::gray32f_view_t::coord_t x = 1; x < harris_response.width() - 1; ++x)
        {
            auto value = [](gil::gray32f_pixel_t pixel) {
                return pixel.at(std::integral_constant<int, 0>{});
            };
            double values[9] = {
                value(harris_response(x - 1, y - 1)),
                value(harris_response(x, y - 1)),
                value(harris_response(x + 1, y - 1)),
                value(harris_response(x - 1, y)),
                value(harris_response(x, y)),
                value(harris_response(x + 1, y)),
                value(harris_response(x - 1, y + 1)),
                value(harris_response(x, y + 1)),
                value(harris_response(x + 1, y + 1))
            };

            auto maxima = *std::max_element(
                values,
                values + 9,
                [](double lhs, double rhs)
                {
                    return lhs < rhs;
                }
            );

            if (maxima == value(harris_response(x, y))
                && std::count(values, values + 9, maxima) == 1
                && maxima >= harris_response_threshold)
            {
                corner_points.emplace_back(x, y);
            }
        }
    }

    return corner_points;
}

int main(int argc, char* argv[]) {
    if (argc != 5)
    {
        std::cout << "usage: " << argv[0] << " <input.png> <odd-window-size>"
            " <hessian-response-threshold> <output.png>\n";
        return -1;
    }

    std::size_t window_size = std::stoul(argv[2]);
    long hessian_determinant_threshold = std::stol(argv[3]);

    gil::rgb8_image_t input_image;

    gil::read_image(argv[1], input_image, gil::png_tag{});

    auto input_view = gil::view(input_image);
    auto grayscaled = to_grayscale(input_view);
    gil::gray8_image_t smoothed_image(grayscaled.dimensions());
    auto smoothed = gil::view(smoothed_image);
    apply_gaussian_blur(gil::view(grayscaled), smoothed);
    gil::gray16s_image_t x_gradient_image(grayscaled.dimensions());
    gil::gray16s_image_t y_gradient_image(grayscaled.dimensions());

    auto x_gradient = gil::view(x_gradient_image);
    auto y_gradient = gil::view(y_gradient_image);
    auto scharr_x = gil::generate_dx_scharr();
    gil::detail::convolve_2d(smoothed, scharr_x, x_gradient);
    auto scharr_y = gil::generate_dy_scharr();
    gil::detail::convolve_2d(smoothed, scharr_y, y_gradient);

    gil::gray32f_image_t m11(x_gradient.dimensions());
    gil::gray32f_image_t m12_21(x_gradient.dimensions());
    gil::gray32f_image_t m22(x_gradient.dimensions());
    gil::compute_hessian_entries(
        x_gradient,
        y_gradient,
        gil::view(m11),
        gil::view(m12_21),
        gil::view(m22)
    );

    gil::gray32f_image_t hessian_response(x_gradient.dimensions());
    auto gaussian_kernel = gil::generate_gaussian_kernel(window_size, 0.84089642);
    gil::compute_hessian_responses(
        gil::view(m11),
        gil::view(m12_21),
        gil::view(m22),
        gaussian_kernel,
        gil::view(hessian_response)
    );

    auto corner_points = suppress(gil::view(hessian_response), hessian_determinant_threshold);
    for (auto point: corner_points) {
        input_view(point) = gil::rgb8_pixel_t(0, 0, 0);
        input_view(point).at(std::integral_constant<int, 1>{}) = 255;
    }
    gil::write_view(argv[4], input_view, gil::png_tag{});
}
