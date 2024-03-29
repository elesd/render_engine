#include <render_engine/cuda_compute/DistanceFieldKernel.h>

#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <stdexcept>

#include <cub/device/device_radix_sort.cuh>

#include <cstdio>
#include <math.h>

using std::min;
using std::max;

#define CUDA_CHECKED_CALL(exp) {cudaError e = (exp); assert(e == cudaSuccess && #exp);}
#define ENABLE_QUERY_DEBUG false

#define SQ(a) ((a) * (a))
#define COMPONENT_AT(point, idx)  (reinterpret_cast<const uint32_t*>(&(point)))[idx]
namespace
{

    union color_t
    {
        struct
        {
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint8_t a;
        } color;
        uint32_t data;
    };
    constexpr uint32_t g_no_data_value = 0xffffffff;
    constexpr uint32_t g_max_coordinate = 4096;

    constexpr auto g_max_distance = 5000u;

#pragma region Segmentation And MortonCode
    // See more details https://developer.nvidia.com/blog/thinking-parallel-part-iii-tree-construction-gpu/
    // Expands a 10-bit integer into 30 bits
    // by inserting 2 zeros after each bit.
    // A bit more details https://www.forceflow.be/2013/10/07/morton-encodingdecoding-through-bit-interleaving-implementations/
    __device__ unsigned int expandBits(unsigned int v)
    {
        v = (v * 0x00010001u) & 0xFF0000FFu;
        v = (v * 0x00000101u) & 0x0F00F00Fu;
        v = (v * 0x00000011u) & 0xC30C30C3u;
        v = (v * 0x00000005u) & 0x49249249u;
        return v;
    }

    __device__ uint3 projectToIntegerSpace(const float3& p)
    {
        return uint3{
            static_cast<uint32_t>(min(max(p.x * 1024.0f, 0.0f), 1023.0f)),
            static_cast<uint32_t>(min(max(p.y * 1024.0f, 0.0f), 1023.0f)),
            static_cast<uint32_t>(min(max(p.z * 1024.0f, 0.0f), 1023.0f))
        };
    }

    // Calculates a 30-bit Morton code for the
    // given 3D point located within the unit cube [0,1].
    __device__ unsigned int morton3D(const float3& p)
    {
        uint3 a = projectToIntegerSpace(p);
        unsigned int xx = expandBits(a.x);
        unsigned int yy = expandBits(a.y);
        unsigned int zz = expandBits(a.z);
        return xx * 4 + yy * 2 + zz;
    }
    /* See more details in Chun's page https://tmc.web.engr.illinois.edu/pub_ann.html?fbclid=IwAR26zQSewG3ohCCOJMRL7pgGDba58fo28jCuoyvirs9ZnqQUP4bjoV5dyVg
     * Shift is not used, because of the nature of the current implementation.
     * I.e.: Sort is done with a radix sort where the normalized [0,1] numbers are projected to [0, 1023]. In this way
     * the morton code can be stored in 30 bits.
     *
     * Morton code need to be stored because radix sort doesn't work with comparators but with number representations.
     *
     * Thus, any shift which is applied on the numbers on the [0,1023] range will result overflow.
     */
#define msbIsLess(x, y) (x) < (y) && (x) < ((x) ^ (y))

    __device__ int32_t cmpShuffle(const uint3& p, const uint3 q)
    {
        uint32_t less_idx = 0;
        uint32_t less_value = p.x ^ q.x;
        for (uint32_t k = 1; k < 3; ++k)
        {
            const uint32_t value = (COMPONENT_AT(p, k)) ^ (COMPONENT_AT(q, k));

            if (msbIsLess(less_value, value))
            {
                less_idx = k;
                less_value = value;
            }
        }
        return COMPONENT_AT(p, less_idx) - COMPONENT_AT(q, less_idx);
    }

    __global__ void segmentationKernel(cudaSurfaceObject_t input,
                                       uint32_t* d_morton_codes,
                                       uint3* d_image_points,
                                       uint32_t segmentation_threshold)
    {
        const int image_width = blockDim.x * gridDim.x;
        const int image_height = blockDim.y * gridDim.y;
        const int image_depth = blockDim.z * gridDim.z;

        const uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
        const uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
        const uint32_t z = blockIdx.z * blockDim.z + threadIdx.z;

        color_t pixel;
        surf3Dread(&pixel.data, input, x * sizeof(color_t), y, z, cudaBoundaryModeZero);

        const uint32_t flat_coordinate = x + y * image_width + z * (image_width * image_height);
        if (pixel.color.r != pixel.color.g
            || pixel.color.r != pixel.color.b
            || pixel.color.r < segmentation_threshold)
        {
            d_image_points[flat_coordinate] = uint3{ g_max_coordinate, g_max_coordinate, g_max_coordinate };
            d_morton_codes[flat_coordinate] = g_no_data_value;
        }
        else
        {
            const float3 normalized_coordinate{ x / static_cast<float>(image_width),
                y / static_cast<float>(image_height),
                z / static_cast<float>(image_depth) };
            d_image_points[flat_coordinate] = projectToIntegerSpace(normalized_coordinate);
            d_morton_codes[flat_coordinate] = morton3D(normalized_coordinate);
        }
    }
#pragma endregion

    struct AABB
    {
        uint3 min{ g_max_coordinate, g_max_coordinate, g_max_coordinate };
        uint3 max{ 0, 0, 0 };
    };

    struct DebugData
    {
        uint32_t range_begin{};
        uint32_t range_end{};
        uint32_t range_center{};
        uint3 position;
        float3 normalized_position;
        uint32_t depth{};
        bool is_point_to_debug{ false };
    };
    struct QueryData
    {
        float distance{ FLT_MAX };
        float distance_sq{ FLT_MAX };
        uint3 point;
        AABB aabb;
#if ENABLE_QUERY_DEBUG
        DebugData debug_data{};
#endif
    };

#pragma region Approximated Nearest Neighbor

#if ENABLE_QUERY_DEBUG
    __device__ void debugKernel(const QueryData& query_data, uint3* d_coordinates, const char* label = "")
    {
        if (query_data.debug_data.is_point_to_debug == false)
        {
            return;
        }

        printf("  [%d] %d--%d-->%d: {%s} Distance: %0.2f, AABB: (%d,%d,%d)->(%d, %d, %d), check: (%d, %d, %d) current best: (%d, %d, %d)\n",
               query_data.debug_data.depth,
               query_data.debug_data.range_begin,
               query_data.debug_data.range_center,
               query_data.debug_data.range_end,
               label,
               query_data.distance,
               query_data.aabb.min.x,
               query_data.aabb.min.y,
               query_data.aabb.min.z,
               query_data.aabb.max.x,
               query_data.aabb.max.y,
               query_data.aabb.max.z,
               d_coordinates[query_data.debug_data.range_center].x,
               d_coordinates[query_data.debug_data.range_center].y,
               d_coordinates[query_data.debug_data.range_center].z,
               query_data.point.x,
               query_data.point.y,
               query_data.point.z);
    }
#endif
    // comparing to the original implementation shift is not used. It is because all the points are already shifted during projection

    __device__ void check_dist(const uint3& p, const uint3& q, QueryData& query_data)
    {
        float distance_sq = SQ(p.x - q.x);
        distance_sq += SQ(p.y - q.y);
        distance_sq += SQ(p.z - q.z);
        if (distance_sq < query_data.distance_sq)
        {
            query_data.distance_sq = distance_sq;
            query_data.distance = std::sqrt(distance_sq);
            query_data.point = p;

            const float int_distance = std::ceil(query_data.distance);
            query_data.aabb.min.x = max(q.x - int_distance, 0.0f);
            query_data.aabb.min.y = max(q.y - int_distance, 0.0f);
            query_data.aabb.min.z = max(q.z - int_distance, 0.0f);

            query_data.aabb.min.x = min(q.x + int_distance, float(g_max_distance));
            query_data.aabb.min.y = min(q.y + int_distance, float(g_max_distance));
            query_data.aabb.min.z = min(q.z + int_distance, float(g_max_distance));

            /*query_data.aabb.min.x = q.x > query_data.distance ? q.x - std::ceil(query_data.distance) : 0;
            query_data.aabb.min.y = q.y > query_data.distance ? q.y - std::ceil(query_data.distance) : 0;
            query_data.aabb.min.z = q.z > query_data.distance ? q.z - std::ceil(query_data.distance) : 0;

            query_data.aabb.max.x = q.x + query_data.distance < g_max_distance ? q.x + std::ceil(query_data.distance) : g_max_distance;
            query_data.aabb.max.y = q.y + query_data.distance < g_max_distance ? q.y + std::ceil(query_data.distance) : g_max_distance;
            query_data.aabb.max.z = q.z + query_data.distance < g_max_distance ? q.z + std::ceil(query_data.distance) : g_max_distance;*/
        }
    }

    // comparing to the original implementation shift is not used. It is because all the points are already shifted during projection
    __device__ float dist_sq_to_box(const uint3& q, const uint3& p1, const uint3& p2)
    {

        uint32_t less_value = p1.x ^ p2.x;
        for (uint32_t k = 1; k < 3; ++k)
        {
            const uint32_t value = (COMPONENT_AT(p1, k)) ^ (COMPONENT_AT(p2, k));
            if (msbIsLess(less_value, value))
            {
                less_value = value;
            }
        }
        int32_t normalization_exponent = 0;
        frexp(float(less_value), &normalization_exponent);
        float distance = 0.0f;
        for (uint32_t j = 0; j < 3; j++)
        {
            const uint32_t p1_bottom = ((COMPONENT_AT(p1, j)) >> normalization_exponent) << normalization_exponent;
            const uint32_t p1_up = p1_bottom + (1 << normalization_exponent);

            if (COMPONENT_AT(q, j) < p1_bottom)
            {
                distance += SQ(COMPONENT_AT(q, j) - p1_bottom);
            }
            else if (COMPONENT_AT(q, j) > p1_up)
            {
                distance += SQ(COMPONENT_AT(q, j) - p1_up);
            }
        }
        return distance;
    }

    __device__ void query_point(uint3* d_coordinates,
                                uint32_t range_begin,
                                uint32_t range_end,
                                uint3 point,
                                float epsilon_distance,
                                QueryData& out_result)
    {


        const uint32_t range_length = range_end - range_begin;
        if (range_length == 0)
        {
            return;
        }
        const uint32_t range_center = range_begin + range_length / 2;


        check_dist(d_coordinates[range_center], point, out_result);
        if (range_length == 1
            || dist_sq_to_box(point, d_coordinates[range_begin], d_coordinates[range_end - 1]) * SQ(1 + epsilon_distance) > out_result.distance_sq)
        {
            return;
        }
        if (cmpShuffle(point, d_coordinates[range_center]) < 0)
        {
            query_point(d_coordinates,
                        range_begin,
                        range_center,
                        point,
                        epsilon_distance,
                        out_result);

            if (cmpShuffle(out_result.aabb.max, d_coordinates[range_center]) > 0)
            {
                query_point(d_coordinates,
                            range_center + 1,
                            range_end,
                            point,
                            epsilon_distance,
                            out_result);
            }

        }
        else
        {
            query_point(d_coordinates,
                        range_center + 1,
                        range_end,
                        point,
                        epsilon_distance,
                        out_result);

            if (cmpShuffle(out_result.aabb.min, d_coordinates[range_center]) < 0)
            {
                query_point(d_coordinates,
                            range_begin,
                            range_center,
                            point,
                            epsilon_distance,
                            out_result);

            }
        }
    }

    __device__ void query_point_no_recursion(uint3* d_coordinates,
                                             uint32_t in_range_begin,
                                             uint32_t in_range_end,
                                             uint3 point,
                                             float epsilon_distance,
                                             QueryData& out_result)
    {
        enum class Phase : uint8_t
        {
            None = 0,
            CheckLowerRange_1,
            CheckUpperRange_1,
            CheckLowerRange_2,
            CheckUpperRange_2
        };
        struct SearchState
        {
            uint32_t range_begin{};
            uint32_t range_end{};
            Phase phase{ Phase::None };
        };
        constexpr int8_t max_depth = 25;
        SearchState search_space[max_depth] = {};
        int8_t current_depth = 0;
        search_space[current_depth] = SearchState{ in_range_begin, in_range_end, Phase::None };

        while (current_depth >= 0)
        {
            auto& search_data = search_space[current_depth];
            const uint32_t range_length = search_data.range_end - search_data.range_begin;

            if (range_length == 0)
            {
                current_depth--;
                continue;
            }
            const uint32_t range_center = search_data.range_begin + range_length / 2;
            if (search_data.phase == Phase::None)
            {
                check_dist(d_coordinates[range_center], point, out_result);
                if (range_length == 1
                    || dist_sq_to_box(point, d_coordinates[search_data.range_begin], d_coordinates[search_data.range_end - 1]) * SQ(1 + epsilon_distance) > out_result.distance_sq)
                {
                    current_depth--;
                    continue;
                }
            }
            const int32_t cmp_result = cmpShuffle(point, d_coordinates[range_center]);
            if (cmp_result < 0)
            {
                assert(search_data.phase != Phase::CheckLowerRange_2
                       && search_data.phase != Phase::CheckUpperRange_2);
                switch (search_data.phase)
                {
                    case Phase::None:
                        search_data.phase = Phase::CheckLowerRange_1;
                        break;
                    case Phase::CheckLowerRange_1:
                        if (cmpShuffle(out_result.aabb.max, d_coordinates[range_center]) > 0)
                        {
                            search_data.phase = Phase::CheckUpperRange_1;
                        }
                        else
                        {
                            search_data.phase = Phase::None;
                        }
                        break;
                    case Phase::CheckUpperRange_1:
                        search_data.phase = Phase::None;
                        break;
                }
            }
            else
            {
                assert(search_data.phase != Phase::CheckLowerRange_1
                       && search_data.phase != Phase::CheckUpperRange_1);

                switch (search_data.phase)
                {
                    case Phase::None:
                        search_data.phase = Phase::CheckUpperRange_2;
                        break;
                    case Phase::CheckUpperRange_2:
                        if (cmpShuffle(out_result.aabb.min, d_coordinates[range_center]) < 0)
                        {
                            search_data.phase = Phase::CheckLowerRange_2;
                        }
                        else
                        {
                            search_data.phase = Phase::None;
                        }
                        break;
                    case Phase::CheckLowerRange_2:
                        search_data.phase = Phase::None;
                        break;
                }
            }

            switch (search_data.phase)
            {
                case Phase::None:
                    current_depth--;
                    break;
                case Phase::CheckUpperRange_1:
                case Phase::CheckUpperRange_2:
                    current_depth++;
                    assert(current_depth < max_depth);
                    search_space[current_depth] = {
                        range_center + 1,
                        search_data.range_end,
                        Phase::None
                    };
                    break;
                case Phase::CheckLowerRange_1:
                case Phase::CheckLowerRange_2:
                    current_depth++;
                    assert(current_depth < max_depth);
                    search_space[current_depth] = {
                        search_data.range_begin,
                        range_center,
                        Phase::None
                    };
                    break;
                default:
                    break;
            }
        }
    }

    __global__ void
        distanceFieldKernel(uint3* d_coordinates,
                            uint32_t point_count,
                            float epsilon_distance,
                            cudaSurfaceObject_t output)
    {
        const int image_width = blockDim.x * gridDim.x;
        const int image_height = blockDim.y * gridDim.y;
        const int image_depth = blockDim.z * gridDim.z;

        const uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
        const uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
        const uint32_t z = blockIdx.z * blockDim.z + threadIdx.z;

        const float3 normalized_p{ x / static_cast<float>(image_width),
        y / static_cast<float>(image_height),
        z / static_cast<float>(image_depth) };
        QueryData result{};
        uint3 p = projectToIntegerSpace(normalized_p);


        query_point_no_recursion(d_coordinates,
                                 0,
                                 point_count,
                                 p,
                                 epsilon_distance,
                                 result);
        surf3Dwrite(result.distance / 1023.0f, output, x * 4, y, z, cudaBoundaryModeZero);
    }
#pragma endregion
}

namespace RenderEngine
{
    namespace CudaCompute
    {

        cudaArray_t DistanceFieldKernel::allocateInputMemory(uint32_t width, uint32_t height, uint32_t depth)
        {

            cudaArray_t device_data{ nullptr };
            cudaChannelFormatDesc channel_format = cudaCreateChannelDesc(8, 8, 8, 8, cudaChannelFormatKindUnsigned);
            CUDA_CHECKED_CALL(cudaMalloc3DArray(&device_data,
                                                &channel_format,
                                                cudaExtent{ width, height, depth },
                                                cudaArraySurfaceLoadStore));

            return device_data;
        }
        cudaArray_t DistanceFieldKernel::allocateOutputMemory(uint32_t width, uint32_t height, uint32_t depth)
        {
            cudaArray_t device_data{ nullptr };
            cudaChannelFormatDesc channel_format = cudaCreateChannelDesc(32, 0, 0, 0, cudaChannelFormatKindFloat);

            CUDA_CHECKED_CALL(cudaMalloc3DArray(&device_data,
                                                &channel_format,
                                                cudaExtent{ width, height, depth },
                                                cudaArraySurfaceLoadStore));

            return device_data;
        }
        void DistanceFieldKernel::freeMemory(cudaArray_t device_memory)
        {
            CUDA_CHECKED_CALL(cudaFreeArray(device_memory));
        }
        DistanceFieldKernel::DistanceFieldKernel(const KernelParameters& kernel_parameters,
                                                 cudaStream_t cuda_stream)
            : _kernel_parameters(kernel_parameters)
            , _cuda_stream(cuda_stream)
        {}

        void DistanceFieldKernel::run(const cudaSurfaceObject_t d_input_data,
                                      cudaSurfaceObject_t d_output_data,
                                      uint32_t segmentation_threshold,
                                      float epsilon_distance)
        {
            const uint32_t width = _kernel_parameters.block_size.x * _kernel_parameters.grid_size.x;
            const uint32_t height = _kernel_parameters.block_size.y * _kernel_parameters.grid_size.y;
            const uint32_t depth = _kernel_parameters.block_size.z * _kernel_parameters.grid_size.z;
            uint32_t num_items = width * height * depth;



            uint32_t* d_kd_tree_morton_codes{ nullptr };
            CUDA_CHECKED_CALL(cudaMalloc(&d_kd_tree_morton_codes, num_items * sizeof(uint32_t)));
            uint3* d_kd_tree_coordinates{ nullptr };
            CUDA_CHECKED_CALL(cudaMalloc(&d_kd_tree_coordinates, num_items * sizeof(uint3)));
            segmentationKernel << <_kernel_parameters.grid_size, _kernel_parameters.block_size, 0, _cuda_stream >> > (
                d_input_data,
                d_kd_tree_morton_codes,
                d_kd_tree_coordinates,
                segmentation_threshold);

            uint32_t* d_kd_tree_morton_codes_sorted{ nullptr };
            CUDA_CHECKED_CALL(cudaMalloc(&d_kd_tree_morton_codes_sorted, num_items * sizeof(uint3)));
            uint3* d_kd_tree_coordinates_sorted{ nullptr };
            CUDA_CHECKED_CALL(cudaMalloc(&d_kd_tree_coordinates_sorted, num_items * sizeof(uint3)));
            cub::DeviceRadixSort radix_sort;
            size_t temporary_size = 0;
            uint32_t* d_temporary_memory = nullptr;
            CUDA_CHECKED_CALL(radix_sort.SortPairs(nullptr,
                                                   temporary_size,
                                                   d_kd_tree_morton_codes,
                                                   d_kd_tree_morton_codes_sorted,
                                                   d_kd_tree_coordinates,
                                                   d_kd_tree_coordinates_sorted,
                                                   num_items));

            CUDA_CHECKED_CALL(cudaMalloc(&d_temporary_memory, temporary_size));

            CUDA_CHECKED_CALL(radix_sort.SortPairs(d_temporary_memory,
                                                   temporary_size,
                                                   d_kd_tree_morton_codes,
                                                   d_kd_tree_morton_codes_sorted,
                                                   d_kd_tree_coordinates,
                                                   d_kd_tree_coordinates_sorted,
                                                   num_items,
                                                   0,
                                                   sizeof(uint32_t) * 8,
                                                   _cuda_stream));


            std::vector<uint32_t> kd_tree_morton_codes_sorted(num_items, uint32_t{});
            CUDA_CHECKED_CALL(cudaMemcpy(kd_tree_morton_codes_sorted.data(), d_kd_tree_morton_codes_sorted, num_items * sizeof(uint32_t), cudaMemcpyDeviceToHost));

            const uint32_t num_of_zeros_at_end = std::find_if(kd_tree_morton_codes_sorted.rbegin(), kd_tree_morton_codes_sorted.rend(),
                                                              [](uint32_t morton_code) { return morton_code != g_no_data_value; })
                - kd_tree_morton_codes_sorted.rbegin();


            CUDA_CHECKED_CALL(cudaFree(d_temporary_memory));
            CUDA_CHECKED_CALL(cudaFree(d_kd_tree_coordinates));
            CUDA_CHECKED_CALL(cudaFree(d_kd_tree_morton_codes));
            CUDA_CHECKED_CALL(cudaFree(d_kd_tree_morton_codes_sorted));


            distanceFieldKernel << <_kernel_parameters.grid_size, _kernel_parameters.block_size, 0, _cuda_stream >> > (
                d_kd_tree_coordinates_sorted,
                num_items - num_of_zeros_at_end,
                epsilon_distance,
                d_output_data);
            CUDA_CHECKED_CALL(cudaFree(d_kd_tree_coordinates_sorted));

        }
    }
}