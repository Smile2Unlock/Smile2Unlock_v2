
#include <fstream>
inline bool SaveRGB24ToBMP(const char* filepath, int width, int height, const unsigned char* rgb_data) {
    std::ofstream out(filepath, std::ios::binary);
    if (!out) return false;
    
    int row_stride = (width * 3 + 3) & ~3; // 4-byte aligned
    int image_size = row_stride * height;
    
    unsigned char file_header[14] = {
        'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0
    };
    int file_size = 54 + image_size;
    file_header[2] = (unsigned char)(file_size);
    file_header[3] = (unsigned char)(file_size >> 8);
    file_header[4] = (unsigned char)(file_size >> 16);
    file_header[5] = (unsigned char)(file_size >> 24);
    
    unsigned char info_header[40] = {
        40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 24,0
    };
    info_header[4] = (unsigned char)(width);
    info_header[5] = (unsigned char)(width >> 8);
    info_header[6] = (unsigned char)(width >> 16);
    info_header[7] = (unsigned char)(width >> 24);
    
    info_header[8] = (unsigned char)(-height); // top-down if negative, but wait, usually negative means top-down. 
    info_header[9] = (unsigned char)((-height) >> 8);
    info_header[10] = (unsigned char)((-height) >> 16);
    info_header[11] = (unsigned char)((-height) >> 24);

    out.write((char*)file_header, 14);
    out.write((char*)info_header, 40);
    
    std::vector<unsigned char> padded_row(row_stride, 0);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src_row = rgb_data + y * width * 3;
        for (int x = 0; x < width; ++x) {
            padded_row[x*3 + 0] = src_row[x*3 + 2]; // B
            padded_row[x*3 + 1] = src_row[x*3 + 1]; // G
            padded_row[x*3 + 2] = src_row[x*3 + 0]; // R
        }
        out.write((char*)padded_row.data(), row_stride);
    }
    return true;
}
