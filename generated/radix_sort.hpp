#pragma once

#include <glad/glad.h>

#include <array>
#include <fstream>
#include <filesystem>
#include <iostream>

#define RGC_RADIX_SORT_THREADS_PER_BLOCK 64
#define RGC_RADIX_SORT_ITEMS_PER_THREAD  4
#define RGC_RADIX_SORT_BITSET_NUM   4
#define RGC_RADIX_SORT_BITSET_COUNT ((sizeof(GLuint) * 8) / RGC_RADIX_SORT_BITSET_NUM)
#define RGC_RADIX_SORT_BITSET_SIZE  GLuint(exp2(RGC_RADIX_SORT_BITSET_NUM))

#ifdef RGC_RADIX_SORT_DEBUG
	#include "renderdoc.hpp"
	#define RGC_RADIX_SORT_RENDERDOC_WATCH(capture, f) rgc::renderdoc::watch(capture, f)
#else
	#define RGC_RADIX_SORT_RENDERDOC_WATCH(capture, f)
#endif

inline void __rgc_shader_injector_load_src(GLuint shader, char const* src)
{
	glShaderSource(shader, 1, &src, nullptr);
}

inline void __rgc_shader_injector_load_src_from_file(GLuint shader, std::filesystem::path const& filename)
{
	std::ifstream file(filename);
	if (!file.is_open())
	{
		std::cerr << "Failed to open file at: " << filename << std::endl;
		throw std::invalid_argument("Failed to open text file.");
	}

	std::string src((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	__rgc_shader_injector_load_src(shader, src.c_str());
}

#define RGC_SHADER_INJECTOR_INJECTION_POINT
#define RGC_SHADER_INJECTOR_LOAD_SRC(shader, path) __rgc_shader_injector_load_src_from_file(shader, path)

const char* __rgc_shader_injector_shader_src_e6da6ab5dc2853d416b4b2047d13533c = "#version 460\n\n#define THREAD_IDX        gl_LocalInvocationIndex\n#define THREADS_NUM       64\n#define THREAD_BLOCK_IDX  (gl_WorkGroupID.x + gl_NumWorkGroups.x * (gl_WorkGroupID.y + gl_NumWorkGroups.z * gl_WorkGroupID.z))\n#define THREAD_BLOCKS_NUM (gl_NumWorkGroups.x * gl_NumWorkGroups.y * gl_NumWorkGroups.z)\n#define ITEMS_NUM         4\n\n#define BITSET_NUM        4\n#define BITSET_SIZE       uint(exp2(BITSET_NUM))\n\nlayout(local_size_x = THREADS_NUM, local_size_y = 1, local_size_z = 1) in;\n\nlayout(std430, binding = 0) buffer ssbo_key           { uint b_key_buf[];  };\nlayout(std430, binding = 1) buffer ssbo_count_buf     { uint b_count_buf[]; }; // [THREAD_BLOCKS_NUM * BITSET_SIZE]\nlayout(std430, binding = 2) buffer ssbo_tot_count_buf { uint b_tot_count_buf[BITSET_SIZE]; };\n\nuniform uint u_arr_len;\nuniform uint u_bitset_idx;\n\nuint to_partition_radixes_offsets_idx(uint radix, uint thread_block_idx)\n{\n    uint pow_of_2_thread_blocks_num = uint(exp2(ceil(log2(THREAD_BLOCKS_NUM))));\n    return radix * pow_of_2_thread_blocks_num + thread_block_idx;\n}\n\nuint to_loc_idx(uint item_idx, uint thread_idx)\n{\n    return (thread_idx * ITEMS_NUM + item_idx);\n}\n\nuint to_key_idx(uint item_idx, uint thread_idx, uint thread_block_idx)\n{\n    return (thread_block_idx * ITEMS_NUM * THREADS_NUM) + (thread_idx * ITEMS_NUM) + item_idx;\n}\n\nvoid main()\n{\n    for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n    {\n        uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n        if (key_idx >= u_arr_len) {\n            continue;\n        }\n\n        uint bitset_mask = (BITSET_SIZE - 1) << (BITSET_NUM * u_bitset_idx);\n        uint rad = (b_key_buf[key_idx] & bitset_mask) >> (BITSET_NUM * u_bitset_idx);\n\n        atomicAdd(b_count_buf[to_partition_radixes_offsets_idx(rad, THREAD_BLOCK_IDX)], 1);\n        atomicAdd(b_tot_count_buf[rad], 1);\n    }\n}\n";
const char* __rgc_shader_injector_shader_src_adcc7bff7b5d808d190d59c89b7f6164 = "#version 460\n\n#define THREAD_IDX        gl_LocalInvocationIndex\n#define THREADS_NUM       64\n#define THREAD_BLOCK_IDX  (gl_WorkGroupID.x + gl_NumWorkGroups.x * (gl_WorkGroupID.y + gl_NumWorkGroups.z * gl_WorkGroupID.z))\n#define ITEMS_NUM         4\n#define BITSET_NUM        4\n#define BITSET_SIZE       uint(exp2(BITSET_NUM))\n\n#define OP_UPSWEEP    0\n#define OP_CLEAR_LAST 1\n#define OP_DOWNSWEEP  2\n\nlayout(local_size_x = THREADS_NUM, local_size_y = 1, local_size_z = 1) in;\n\nlayout(std430, binding = 0) buffer ssbo_local_offsets_buf { uint b_local_offsets_buf[]; }; // b_count_buf[THREAD_BLOCKS_NUM * BITSET_SIZE]\n\nuniform uint u_arr_len; // Already guaranteed to be a power of 2\nuniform uint u_depth;\nuniform uint u_op;\n\nuint to_partition_radixes_offsets_idx(uint radix, uint thread_block_idx)\n{\n    return radix * u_arr_len + thread_block_idx;\n}\n\nuint to_loc_idx(uint item_idx, uint thread_idx)\n{\n    return (thread_idx * ITEMS_NUM + item_idx);\n}\n\nuint to_key_idx(uint item_idx, uint thread_idx, uint thread_block_idx)\n{\n    return (thread_block_idx * ITEMS_NUM * THREADS_NUM) + (thread_idx * ITEMS_NUM) + item_idx;\n}\n\nvoid main()\n{\n    if (fract(log2(u_arr_len)) != 0) {\n        return; // ERROR: The u_arr_len must be a power of 2 otherwise the Blelloch scan won't work!\n    }\n\n    // ------------------------------------------------------------------------------------------------\n    // Blelloch scan\n    // ------------------------------------------------------------------------------------------------\n\n    uint step = uint(exp2(u_depth));\n\n    if (u_op == OP_UPSWEEP)\n    {\n        // Reduce (upsweep)\n        for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n        {\n            uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n            if (key_idx % (step * 2) == 0)\n            {\n                uint from_idx = key_idx + (step - 1);\n                uint to_idx = from_idx + step;\n\n                if (to_idx < u_arr_len)\n                {\n                    for (uint rad = 0; rad < BITSET_SIZE; rad++)\n                    {\n                        uint from_rad_idx = to_partition_radixes_offsets_idx(rad, from_idx);\n                        uint to_rad_idx = to_partition_radixes_offsets_idx(rad, to_idx);\n\n                        b_local_offsets_buf[to_rad_idx] = b_local_offsets_buf[from_rad_idx] + b_local_offsets_buf[to_rad_idx];\n                    }\n                }\n            }\n        }\n    }\n    else if (u_op == OP_DOWNSWEEP)\n    {\n        // Downsweep\n        for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n        {\n            uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n            if (key_idx % (step * 2) == 0)\n            {\n                uint from_idx = key_idx + (step - 1);\n                uint to_idx = from_idx + step;\n\n                if (to_idx < u_arr_len)\n                {\n                    for (uint rad = 0; rad < BITSET_SIZE; rad++)\n                    {\n                        uint from_rad_idx = to_partition_radixes_offsets_idx(rad, from_idx);\n                        uint to_rad_idx = to_partition_radixes_offsets_idx(rad, to_idx);\n\n                        uint r = b_local_offsets_buf[to_rad_idx];\n                        b_local_offsets_buf[to_rad_idx] = b_local_offsets_buf[from_rad_idx] + b_local_offsets_buf[to_rad_idx];\n                        b_local_offsets_buf[from_rad_idx] = r;\n                    }\n                }\n            }\n        }\n    }\n    else// if (u_op == OP_CLEAR_LAST)\n    {\n        for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n        {\n            uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n            if (key_idx == (u_arr_len - 1))\n            {\n                for (uint rad = 0; rad < BITSET_SIZE; rad++)\n                {\n                    uint idx = to_partition_radixes_offsets_idx(rad, key_idx);\n                    b_local_offsets_buf[idx] = 0;\n                }\n            }\n        }\n    }\n}\n";
const char* __rgc_shader_injector_shader_src_d6a1b28e01eb1b04a76c1e9f75227206 = "#version 460\n\n#define THREAD_IDX        gl_LocalInvocationIndex\n#define THREADS_NUM       64\n#define THREAD_BLOCK_IDX  (gl_WorkGroupID.x + gl_NumWorkGroups.x * (gl_WorkGroupID.y + gl_NumWorkGroups.z * gl_WorkGroupID.z))\n#define THREAD_BLOCKS_NUM (gl_NumWorkGroups.x * gl_NumWorkGroups.y * gl_NumWorkGroups.z)\n#define ITEMS_NUM         4\n#define BITSET_NUM        4\n#define BITSET_SIZE       uint(exp2(BITSET_NUM))\n\nlayout(local_size_x = THREADS_NUM, local_size_y = 1, local_size_z = 1) in;\n\nlayout(std430, binding = 0) restrict readonly buffer in_buffer\n{\n    uint b_in_values[];\n};\n\nlayout(std430, binding = 1) restrict writeonly buffer out_buffer\n{\n    uint b_out_values[];\n};\n\nlayout(std430, binding = 2) buffer ssbo_local_off_buf  { uint b_local_offsets_buf[]; };\nlayout(std430, binding = 3) buffer ssbo_glob_count_buf { uint b_glob_counts_buf[BITSET_SIZE]; }; // todo\n\nuniform uint u_arr_len;\nuniform uint u_bitset_idx;\n\n#define UINT32_MAX uint(-1)\n\nshared uint s_prefix_sum[BITSET_SIZE][THREADS_NUM * ITEMS_NUM];\nshared uint s_key_buf[THREADS_NUM * ITEMS_NUM][2];\nshared uint s_count[BITSET_SIZE];\n\nuint to_partition_radixes_offsets_idx(uint radix, uint thread_block_idx)\n{\n    uint pow_of_2_thread_blocks_num = uint(exp2(ceil(log2(THREAD_BLOCKS_NUM))));\n    return radix * pow_of_2_thread_blocks_num + thread_block_idx;\n}\n\nuint to_loc_idx(uint item_idx, uint thread_idx)\n{\n    return (thread_idx * ITEMS_NUM + item_idx);\n}\n\nuint to_key_idx(uint item_idx, uint thread_idx, uint thread_block_idx)\n{\n    return (thread_block_idx * ITEMS_NUM * THREADS_NUM) + (thread_idx * ITEMS_NUM) + item_idx;\n}\n\nvoid main()\n{\n    // ------------------------------------------------------------------------------------------------\n    // Global offsets\n    // ------------------------------------------------------------------------------------------------\n\n    // Runs an exclusive scan on the global counts to get the starting offset for each radix.\n    // The offsets are global for the whole input array.\n\n    uint glob_off_buf[BITSET_SIZE];\n\n    // Exclusive scan\n    for (uint sum = 0, i = 0; i < BITSET_SIZE; i++)\n    {\n        glob_off_buf[i] = sum;\n        sum += b_glob_counts_buf[i];\n    }\n\n    // ------------------------------------------------------------------------------------------------\n    // Radix sort on partition\n    // ------------------------------------------------------------------------------------------------\n\n    // NOTE:\n    // The last partition potentially can have keys that aren't part of the original array.\n    // These keys during the radix sort are set to `UINT32_MAX` (or 0xFFFFFFFF), this way, after the array is sorted\n    // they'll always be in the last position and won't be confused with the original keys of the input array.\n\n    for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n    {\n        uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n        uint loc_idx = to_loc_idx(item_idx, THREAD_IDX);\n        s_key_buf[loc_idx][0] = key_idx < u_arr_len ? b_in_values[key_idx] : UINT32_MAX; // If the key_idx is outside of the input array then use UINT32_MAX.\n        s_key_buf[loc_idx][1] = UINT32_MAX;\n    }\n\n    barrier();\n\n    // The offsets of the radixes within the partition. The values of this array will be written every radix-sort iteration.\n    uint in_partition_group_off[BITSET_SIZE];\n\n    uint bitset_idx;\n    for (bitset_idx = 0; bitset_idx <= u_bitset_idx; bitset_idx++)\n    {\n        uint bitset_mask = (BITSET_SIZE - 1) << (BITSET_NUM * bitset_idx);\n\n        // Init\n        for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n        {\n            for (uint bitset_val = 0; bitset_val < BITSET_SIZE; bitset_val++)\n            {\n                uint loc_idx = to_loc_idx(item_idx, THREAD_IDX);\n                s_prefix_sum[bitset_val][loc_idx] = 0;\n            }\n        }\n\n        barrier();\n\n        // ------------------------------------------------------------------------------------------------\n        // Predicate test\n        // ------------------------------------------------------------------------------------------------\n\n        // For each key of the current partition sets a 1 whether the radix corresponds to the radix of the current key.\n        // Otherwise the default value is initialized to 0.\n\n        for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n        {\n            uint loc_idx = to_loc_idx(item_idx, THREAD_IDX);\n            uint k = s_key_buf[loc_idx][bitset_idx % 2];\n            uint radix = (k & bitset_mask) >> (BITSET_NUM * bitset_idx);\n            s_prefix_sum[radix][loc_idx] = 1;\n        }\n\n        barrier();\n\n        // ------------------------------------------------------------------------------------------------\n        // Exclusive sum\n        // ------------------------------------------------------------------------------------------------\n\n        // THREADS_NUM * ITEMS_NUM are guaranteed to be a power of two, otherwise it won't work!\n\n        // An exclusive sum is run on the predicates, this way, for each location in the partition\n        // we know the offset the element has to go, relative to its radix.\n\n        // Up-sweep\n        for (uint d = 0; d < uint(log2(THREADS_NUM * ITEMS_NUM)); d++)\n        {\n            for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n            {\n                uint step = uint(exp2(d));\n                uint loc_idx = to_loc_idx(item_idx, THREAD_IDX);\n\n                if (loc_idx % (step * 2) == 0)\n                {\n                    uint from_idx = loc_idx + (step - 1);\n                    uint to_idx = from_idx + step;\n\n                    if (to_idx < THREADS_NUM * ITEMS_NUM)\n                    {\n                        for (uint bitset_val = 0; bitset_val < BITSET_SIZE; bitset_val++)\n                        {\n                            s_prefix_sum[bitset_val][to_idx] = s_prefix_sum[bitset_val][from_idx] + s_prefix_sum[bitset_val][to_idx];\n                        }\n                    }\n                }\n            }\n\n            barrier();\n        }\n\n        // Clear last\n        if (THREAD_IDX == 0)\n        {\n            for (uint bitset_val = 0; bitset_val < BITSET_SIZE; bitset_val++)\n            {\n                s_prefix_sum[bitset_val][(THREADS_NUM * ITEMS_NUM) - 1] = 0;\n            }\n        }\n\n        barrier();\n\n        // Down-sweep\n        for (int d = int(log2(THREADS_NUM * ITEMS_NUM)) - 1; d >= 0; d--)\n        {\n            for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n            {\n                uint step = uint(exp2(d));\n                uint loc_idx = to_loc_idx(item_idx, THREAD_IDX);\n\n                if (loc_idx % (step * 2) == 0)\n                {\n                    uint from_idx = loc_idx + (step - 1);\n                    uint to_idx = from_idx + step;\n\n                    if (to_idx < THREADS_NUM * ITEMS_NUM)\n                    {\n                        for (uint bitset_val = 0; bitset_val < BITSET_SIZE; bitset_val++)\n                        {\n                            uint r = s_prefix_sum[bitset_val][to_idx];\n                            s_prefix_sum[bitset_val][to_idx] = r + s_prefix_sum[bitset_val][from_idx];\n                            s_prefix_sum[bitset_val][from_idx] = r;\n                        }\n                    }\n                }\n            }\n\n            barrier();\n        }\n\n        // ------------------------------------------------------------------------------------------------\n        // Shuffling\n        // ------------------------------------------------------------------------------------------------\n\n        uint last_loc_idx;\n        if (THREAD_BLOCK_IDX == (THREAD_BLOCKS_NUM - 1)) { // The last partition may be larger than the original size of the array, so the last position is before its end.\n            last_loc_idx = u_arr_len - (THREAD_BLOCKS_NUM - 1) * (THREADS_NUM * ITEMS_NUM) - 1;\n        } else {\n            last_loc_idx = (THREADS_NUM * ITEMS_NUM) - 1;\n        }\n\n        // Now for every radix we need to know its "global" offset within the partition (we only know relative offsets per location within the same radix).\n        // So we just issue an exclusive scan (again) on the last element offset of every radix.\n\n        for (uint sum = 0, i = 0; i < BITSET_SIZE; i++) // Small prefix-sum to calculate group offsets.\n        {\n            in_partition_group_off[i] = sum;\n\n            // Since we need the count of every bit-group, if the last element is the bitset value itself, we need to count it.\n            // Otherwise it will be already counted.\n            bool is_last = ((s_key_buf[last_loc_idx][bitset_idx % 2] & bitset_mask) >> (BITSET_NUM * bitset_idx)) == i;\n            sum += s_prefix_sum[i][last_loc_idx] + (is_last ? 1 : 0);\n        }\n\n        for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n        {\n            uint loc_idx = to_loc_idx(item_idx, THREAD_IDX);\n            uint k = s_key_buf[loc_idx][bitset_idx % 2];\n            uint radix = (k & bitset_mask) >> (BITSET_NUM * bitset_idx);\n\n            // The destination address is calculated as the global offset of the radix plus the\n            // local offset that depends by the location of the element.\n            uint dest_addr = in_partition_group_off[radix] + s_prefix_sum[radix][loc_idx];\n            s_key_buf[dest_addr][(bitset_idx + 1) % 2] = k; // Double-buffering is used every cycle the read and write buffer are swapped.\n        }\n\n        barrier();\n    }\n\n    // TODO (REMOVE) WRITE s_prefix_sum\n    /*\n    for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n    {\n        uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n        if (item_idx < u_arr_len)\n        {\n            uint loc_idx = to_loc_idx(item_idx, THREAD_IDX);\n            b_out_values[key_idx] = s_prefix_sum[1][loc_idx];\n        }\n    }\n\n    for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n    {\n        uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n        if (key_idx < u_arr_len)\n        {\n            uint loc_idx = to_loc_idx(item_idx, THREAD_IDX);\n            b_out_values[key_idx] = s_key_buf[loc_idx][bitset_idx % 2];\n        }\n    }\n    */\n\n    // ------------------------------------------------------------------------------------------------\n    // Scattered writes to sorted partitions\n    // ------------------------------------------------------------------------------------------------\n\n    // TODO DUPLICATION ISSUE IS INVOLVED HERE!\n\n    uint bitset_mask = (BITSET_SIZE - 1) << (BITSET_NUM * u_bitset_idx);\n\n    /*\n    if (THREAD_IDX == 0)\n    {\n        for (uint i = 0; i < BITSET_SIZE; i++) {\n            s_count[i] = 0;\n        }\n    }\n\n    barrier();\n\n    for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n    {\n        uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n        if (key_idx < u_arr_len)\n        {\n            uint k = s_key_buf[to_loc_idx(item_idx, THREAD_IDX)][bitset_idx % 2];\n            uint rad = (k & bitset_mask) >> (BITSET_NUM * u_bitset_idx);\n\n            atomicAdd(s_count[rad], 1);\n        }\n    }\n\n    barrier();\n\n    for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n    {\n        uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n        if (key_idx < u_arr_len)\n        {\n            uint k = s_key_buf[to_loc_idx(item_idx, THREAD_IDX)][bitset_idx % 2];\n            uint rad = (k & bitset_mask) >> (BITSET_NUM * u_bitset_idx);\n\n            b_out_values[key_idx] = s_count[rad];\n        }\n    }\n\n    barrier();\n    */\n\n    for (uint item_idx = 0; item_idx < ITEMS_NUM; item_idx++)\n    {\n        uint key_idx = to_key_idx(item_idx, THREAD_IDX, THREAD_BLOCK_IDX);\n        if (key_idx < u_arr_len)\n        {\n            uint loc_idx = to_loc_idx(item_idx, THREAD_IDX);\n            uint k = s_key_buf[loc_idx][bitset_idx % 2];\n            uint rad = (k & bitset_mask) >> (BITSET_NUM * u_bitset_idx);\n\n            uint glob_off = glob_off_buf[rad];\n            uint local_off = b_local_offsets_buf[to_partition_radixes_offsets_idx(rad, THREAD_BLOCK_IDX)];\n\n            uint dest_idx = glob_off + local_off + (loc_idx - in_partition_group_off[rad]);\n            b_out_values[dest_idx] = k;\n        }\n    }\n}\n";


namespace rgc::radix_sort
{
	const GLuint k_zero = 0;

	struct shader
	{
		GLuint m_name;

		shader(GLenum type)
		{
			m_name = glCreateShader(type);
		}

		~shader()
		{
			glDeleteShader(m_name);
		}

		std::string get_info_log() const
		{
			GLint max_len = 0;
			glGetShaderiv(m_name, GL_INFO_LOG_LENGTH, &max_len);

			std::vector<GLchar> log(max_len);
			glGetShaderInfoLog(m_name, max_len, nullptr, log.data());
			return std::string(log.begin(), log.end());
		}

		void compile() const
		{
			glCompileShader(m_name);

			GLint status;
			glGetShaderiv(m_name, GL_COMPILE_STATUS, &status);
			if (status == GL_FALSE)
			{
				std::cerr << "Failed to compile:\n" << get_info_log() << std::endl;
				throw std::runtime_error("Couldn't compile shader. See console logs for more information.");
			}
		}

	};

	struct program
	{
		GLuint m_name;

		program()
		{
			m_name = glCreateProgram();
		}

		~program()
		{
			glDeleteProgram(m_name);
		}

		void attach_shader(GLuint shader) const
		{
			glAttachShader(m_name, shader);
		}

		std::string get_info_log() const
		{
			GLint log_len = 0;
			glGetProgramiv(m_name, GL_INFO_LOG_LENGTH, &log_len);

			std::vector<GLchar> log(log_len);
			glGetProgramInfoLog(m_name, log_len, nullptr, log.data());
			return std::string(log.begin(), log.end());
		}

		void link() const
		{
			GLint status;
			glLinkProgram(m_name);
			glGetProgramiv(m_name, GL_LINK_STATUS, &status);
			if (status == GL_FALSE)
			{
				std::cerr << get_info_log() << std::endl;
				throw std::runtime_error("Couldn't link shader program.");
			}
		}

		GLint get_uniform_location(const char* name) const
		{
			GLint loc = glGetUniformLocation(m_name, name);
			if (loc < 0) {
				throw std::runtime_error("Couldn't find uniform. Is it unused maybe?");
			}
			return loc;
		}

		void use() const
		{
			glUseProgram(m_name);
		}

		static void unuse()
		{
			glUseProgram(NULL);
		}
	};

	struct sorter
	{
	private:
		program m_count_program;
		program m_local_offsets_program;
		program m_reorder_program;

		size_t m_internal_arr_len;

		GLuint m_local_offsets_buf;
		GLuint m_scratch_buf;
		GLuint m_glob_counts_buf;

		GLuint calc_thread_blocks_num(size_t arr_len)
		{
			return GLuint(ceil(float(arr_len) / float(RGC_RADIX_SORT_THREADS_PER_BLOCK * RGC_RADIX_SORT_ITEMS_PER_THREAD)));
		}

		template<typename T>
		T round_to_power_of_2(T dim)
		{
			return (T) exp2(ceil(log2(dim)));
		}

		void resize_internal_buf(size_t arr_len)
		{
			if (m_local_offsets_buf != NULL) glDeleteBuffers(1, &m_local_offsets_buf);
			if (m_glob_counts_buf != NULL) glDeleteBuffers(1, &m_local_offsets_buf);
			if (m_scratch_buf != NULL) glDeleteBuffers(1, &m_local_offsets_buf);

			m_internal_arr_len = arr_len;

			glGenBuffers(1, &m_local_offsets_buf); // TODO TRY TO REMOVE THIS BUFFER
			glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_local_offsets_buf);
			glBufferStorage(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(round_to_power_of_2(calc_thread_blocks_num(arr_len)) * RGC_RADIX_SORT_BITSET_SIZE * sizeof(GLuint)), nullptr, GL_DYNAMIC_STORAGE_BIT);

			glGenBuffers(1, &m_glob_counts_buf);
			glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_glob_counts_buf);
			glBufferStorage(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(RGC_RADIX_SORT_BITSET_SIZE * sizeof(GLuint)), nullptr, NULL);

			glGenBuffers(1, &m_scratch_buf);
			glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_scratch_buf);
			glBufferStorage(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr) (arr_len * sizeof(GLuint)), nullptr, GL_DYNAMIC_STORAGE_BIT);
		}

	public:
		sorter(size_t init_arr_len)
		{
			{
				shader sh(GL_COMPUTE_SHADER);
				__rgc_shader_injector_load_src(sh.m_name, __rgc_shader_injector_shader_src_e6da6ab5dc2853d416b4b2047d13533c);
				sh.compile();

				m_count_program.attach_shader(sh.m_name);
				m_count_program.link();
			}

			{
				shader sh(GL_COMPUTE_SHADER);
				__rgc_shader_injector_load_src(sh.m_name, __rgc_shader_injector_shader_src_adcc7bff7b5d808d190d59c89b7f6164);
				sh.compile();

				m_local_offsets_program.attach_shader(sh.m_name);
				m_local_offsets_program.link();
			}

			{
				shader sh(GL_COMPUTE_SHADER);
				__rgc_shader_injector_load_src(sh.m_name, __rgc_shader_injector_shader_src_d6a1b28e01eb1b04a76c1e9f75227206);
				sh.compile();

				m_reorder_program.attach_shader(sh.m_name);
				m_reorder_program.link();
			}

			resize_internal_buf(init_arr_len);
		}

		~sorter()
		{
			if (m_local_offsets_buf != NULL) glDeleteBuffers(1, &m_local_offsets_buf);
			if (m_glob_counts_buf != NULL) glDeleteBuffers(1, &m_local_offsets_buf);
			if (m_scratch_buf != NULL) glDeleteBuffers(1, &m_local_offsets_buf);
		}

		void sorter::sort(GLuint key_buf, GLuint val_buf, size_t arr_len)
		{
			if (arr_len <= 1) {
				return;
			}

			if (m_internal_arr_len < arr_len) {
				resize_internal_buf(arr_len);
			}

			// ------------------------------------------------------------------------------------------------
			// Sorting
			// ------------------------------------------------------------------------------------------------

			GLuint thread_blocks_num = calc_thread_blocks_num(arr_len);
			GLuint power_of_two_thread_blocks_num = round_to_power_of_2(thread_blocks_num);

			GLuint buffers[2] = { key_buf, m_scratch_buf };

			for (GLuint pass = 0; pass < RGC_RADIX_SORT_BITSET_COUNT; pass++)
			{
				// ------------------------------------------------------------------------------------------------
				// Initial clearing
				// ------------------------------------------------------------------------------------------------

				glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_glob_counts_buf);
				glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED, GL_UNSIGNED_INT, &k_zero);

				glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_local_offsets_buf);
				glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED, GL_UNSIGNED_INT, &k_zero);

				// ------------------------------------------------------------------------------------------------
				// Counting
				// ------------------------------------------------------------------------------------------------

				// Per-block & global radix count

				m_count_program.use();

				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[pass % 2]);
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_local_offsets_buf);
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_glob_counts_buf);

				glUniform1ui(m_count_program.get_uniform_location("u_arr_len"), arr_len);
				glUniform1ui(m_count_program.get_uniform_location("u_bitset_idx"), pass);

				RGC_RADIX_SORT_RENDERDOC_WATCH(true, [&]()
				{
					glDispatchCompute(thread_blocks_num, 1, 1);
					glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
				});

				// ------------------------------------------------------------------------------------------------
				// Local offsets (block-wide exclusive scan per radix)
				// ------------------------------------------------------------------------------------------------

				m_local_offsets_program.use();

				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_local_offsets_buf);

				// Up-sweep (reduction)
				for (GLuint d = 0; d < GLuint(log2(power_of_two_thread_blocks_num)); d++)
				{
					glUniform1ui(m_local_offsets_program.get_uniform_location("u_arr_len"), power_of_two_thread_blocks_num);
					glUniform1ui(m_local_offsets_program.get_uniform_location("u_op"), 0);
					glUniform1ui(m_local_offsets_program.get_uniform_location("u_depth"), d);

					RGC_RADIX_SORT_RENDERDOC_WATCH(false, [&]()
					{
						auto workgroups_num = GLuint(ceil(float(power_of_two_thread_blocks_num) / float(RGC_RADIX_SORT_THREADS_PER_BLOCK * RGC_RADIX_SORT_ITEMS_PER_THREAD)));
						glDispatchCompute(workgroups_num, 1, 1);
						glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
					});
				}

				// Clear last
				glUniform1ui(m_local_offsets_program.get_uniform_location("u_arr_len"), power_of_two_thread_blocks_num);
				glUniform1ui(m_local_offsets_program.get_uniform_location("u_op"), 1);

				RGC_RADIX_SORT_RENDERDOC_WATCH(false, [&]()
				{
					auto workgroups_num = GLuint(ceil(float(power_of_two_thread_blocks_num) / float(RGC_RADIX_SORT_THREADS_PER_BLOCK * RGC_RADIX_SORT_ITEMS_PER_THREAD)));
					glDispatchCompute(workgroups_num, 1, 1);
					glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
				});

				// Down-sweep
				for (GLint d = GLint(log2(power_of_two_thread_blocks_num)) - 1; d >= 0; d--)
				{
					glUniform1ui(m_local_offsets_program.get_uniform_location("u_arr_len"), power_of_two_thread_blocks_num);
					glUniform1ui(m_local_offsets_program.get_uniform_location("u_op"), 2);
					glUniform1ui(m_local_offsets_program.get_uniform_location("u_depth"), d);

					RGC_RADIX_SORT_RENDERDOC_WATCH(false, [&]()
					{
						auto workgroups_num = GLuint(ceil(float(power_of_two_thread_blocks_num) / float(RGC_RADIX_SORT_THREADS_PER_BLOCK * RGC_RADIX_SORT_ITEMS_PER_THREAD)));
						glDispatchCompute(workgroups_num, 1, 1);
						glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
					});
				}

				// ------------------------------------------------------------------------------------------------
				// Reordering
				// ------------------------------------------------------------------------------------------------

				// In thread-block reordering & writes to global memory

				m_reorder_program.use();

				glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, buffers[pass % 2], 0, (GLsizeiptr) (arr_len * sizeof(GLuint)));
				glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, buffers[(pass + 1) % 2], 0, (GLsizeiptr) (arr_len * sizeof(GLuint)));
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_local_offsets_buf);
				glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_glob_counts_buf);

				glUniform1ui(m_reorder_program.get_uniform_location("u_arr_len"), arr_len);
				glUniform1ui(m_reorder_program.get_uniform_location("u_bitset_idx"), pass);

				RGC_RADIX_SORT_RENDERDOC_WATCH(true, [&]()
				{
					glDispatchCompute(thread_blocks_num, 1, 1);
					glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
				});
			}

			program::unuse();
		}
	};
}

