/*
 * Copyright (c) 2017 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/core/CL/kernels/CLBatchNormalizationLayerKernel.h"

#include "arm_compute/core/CL/CLHelpers.h"
#include "arm_compute/core/CL/CLKernelLibrary.h"
#include "arm_compute/core/CL/ICLTensor.h"
#include "arm_compute/core/FixedPoint.h"
#include "arm_compute/core/Helpers.h"
#include "arm_compute/core/TensorInfo.h"
#include "arm_compute/core/Utils.h"
#include "arm_compute/core/Validate.h"
#include "arm_compute/core/Window.h"

#include "support/ToolchainSupport.h"

using namespace arm_compute;

CLBatchNormalizationLayerKernel::CLBatchNormalizationLayerKernel()
    : _input(nullptr), _output(nullptr), _mean(nullptr), _var(nullptr), _beta(nullptr), _gamma(nullptr), _epsilon(0)
{
}

void CLBatchNormalizationLayerKernel::configure(ICLTensor *input, ICLTensor *output, const ICLTensor *mean, const ICLTensor *var, const ICLTensor *beta, const ICLTensor *gamma,
                                                float epsilon)
{
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(input, 1, DataType::QS8, DataType::QS16, DataType::F16, DataType::F32);

    _input   = input;
    _output  = output;
    _mean    = mean;
    _var     = var;
    _beta    = beta;
    _gamma   = gamma;
    _epsilon = epsilon;

    if(output != nullptr)
    {
        // Output tensor auto initialization if not yet initialized
        auto_init_if_empty(*output->info(), input->info()->tensor_shape(), 1, input->info()->data_type(), input->info()->fixed_point_position());

        ARM_COMPUTE_ERROR_ON_MISMATCHING_SHAPES(input, output);
        ARM_COMPUTE_ERROR_ON_MISMATCHING_DATA_TYPES(input, output, mean, var, beta, gamma);
        ARM_COMPUTE_ERROR_ON_MISMATCHING_FIXED_POINT(input, output, mean, var, beta, gamma);
    }
    else
    {
        ARM_COMPUTE_ERROR_ON_MISMATCHING_DATA_TYPES(input, mean, var, beta, gamma);
        ARM_COMPUTE_ERROR_ON_MISMATCHING_FIXED_POINT(input, mean, var, beta, gamma);
    }

    ARM_COMPUTE_ERROR_ON_MISMATCHING_SHAPES(mean, var, beta, gamma);
    ARM_COMPUTE_ERROR_ON(input->info()->dimension(2) != mean->info()->dimension(0));

    const unsigned int num_elems_processed_per_iteration = 16 / input->info()->element_size();

    // Set build options
    std::set<std::string> build_opts;
    build_opts.emplace(("-DDATA_TYPE=" + get_cl_type_from_data_type(input->info()->data_type())));
    build_opts.emplace(("-DVEC_SIZE=" + support::cpp11::to_string(num_elems_processed_per_iteration)));
    build_opts.emplace(output == nullptr ? "-DIN_PLACE" : "");
    if(is_data_type_fixed_point(input->info()->data_type()))
    {
        build_opts.emplace("-DFIXED_POINT_POSITION=" + support::cpp11::to_string(input->info()->fixed_point_position()));
    }

    // Create kernel
    _kernel = static_cast<cl::Kernel>(CLKernelLibrary::get().create_kernel("batchnormalization_layer", build_opts));

    // Set kernel static arguments
    unsigned int idx = 2 * num_arguments_per_3D_tensor() + 4 * num_arguments_per_1D_tensor(); // Skip the input and output parameters
    _kernel.setArg<cl_float>(idx++, _epsilon);

    // Configure kernel window
    Window                 win = calculate_max_window(*input->info(), Steps(num_elems_processed_per_iteration));
    AccessWindowHorizontal input_access(input->info(), 0, num_elems_processed_per_iteration);
    if(output != nullptr)
    {
        AccessWindowHorizontal output_access(output->info(), 0, num_elems_processed_per_iteration);
        update_window_and_padding(win, input_access, output_access);
        output_access.set_valid_region(win, input->info()->valid_region());
    }
    else
    {
        update_window_and_padding(win, input_access);
    }
    ICLKernel::configure(win);
}

void CLBatchNormalizationLayerKernel::run(const Window &window, cl::CommandQueue &queue)
{
    ARM_COMPUTE_ERROR_ON_UNCONFIGURED_KERNEL(this);
    ARM_COMPUTE_ERROR_ON_INVALID_SUBWINDOW(IKernel::window(), window);

    Window slice = window.first_slice_window_3D();

    Window vector_slice = window.first_slice_window_1D();
    vector_slice.set(Window::DimX, Window::Dimension(0, 0, 0));

    unsigned int idx = 2 * num_arguments_per_3D_tensor();
    add_1D_tensor_argument(idx, _mean, vector_slice);
    add_1D_tensor_argument(idx, _var, vector_slice);
    add_1D_tensor_argument(idx, _beta, vector_slice);
    add_1D_tensor_argument(idx, _gamma, vector_slice);

    do
    {
        idx = 0;
        add_3D_tensor_argument(idx, _input, slice);
        if(_output != nullptr)
        {
            add_3D_tensor_argument(idx, _output, slice);
        }
        enqueue(queue, *this, slice);
    }
    while(window.slide_window_slice_3D(slice));
}
