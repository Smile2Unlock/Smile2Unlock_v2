#pragma once

#include <fstream>
#include <vector>

inline bool SaveRGB24ToBMPFile(const char* filepath, int width, int height, const unsigned char* rgb_data) {
    std::ofstream out(filepath, std::ios::binary);
    if (!out) {
        return false;
    }

    const int row_stride = (width * 3 + 3) & ~3;
    const int image_size = row_stride * height;

    unsigned char file_header[14] = {
        'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0
    };
    const int file_size = 54 + image_size;
    file_header[2] = static_cast<unsigned char>(file_size);
    file_header[3] = static_cast<unsigned char>(file_size >> 8);
    file_header[4] = static_cast<unsigned char>(file_size >> 16);
    file_header[5] = static_cast<unsigned char>(file_size >> 24);

    unsigned char info_header[40] = {
        40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 24, 0
    };
    info_header[4] = static_cast<unsigned char>(width);
    info_header[5] = static_cast<unsigned char>(width >> 8);
    info_header[6] = static_cast<unsigned char>(width >> 16);
    info_header[7] = static_cast<unsigned char>(width >> 24);

    const int negative_height = -height;
    info_header[8] = static_cast<unsigned char>(negative_height);
    info_header[9] = static_cast<unsigned char>(negative_height >> 8);
    info_header[10] = static_cast<unsigned char>(negative_height >> 16);
    info_header[11] = static_cast<unsigned char>(negative_height >> 24);

    out.write(reinterpret_cast<char*>(file_header), sizeof(file_header));
    out.write(reinterpret_cast<char*>(info_header), sizeof(info_header));

    std::vector<unsigned char> padded_row(static_cast<size_t>(row_stride), 0);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src_row = rgb_data + y * width * 3;
        for (int x = 0; x < width; ++x) {
            padded_row[x * 3] = src_row[x * 3 + 2];
            padded_row[x * 3 + 1] = src_row[x * 3 + 1];
            padded_row[x * 3 + 2] = src_row[x * 3];
        }
        out.write(reinterpret_cast<char*>(padded_row.data()), row_stride);
    }

    return true;
}
