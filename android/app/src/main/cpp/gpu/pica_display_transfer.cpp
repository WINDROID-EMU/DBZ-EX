#include "pica_display_transfer.h"
#include "../horizon/horizon_os.h"
#include <algorithm>
#include <array>
#include <vector>
#include <limits>
#include <android/log.h>

#define LOG_TAG "PicaTransfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct Color {
    uint8_t R = 0;
    uint8_t G = 0;
    uint8_t B = 0;
    uint8_t A = 0;
};

void SetError(std::string* error, const char* message) {
    if (error) {
        *error = message;
    }
}

uint8_t Expand4(uint32_t value) {
    return static_cast<uint8_t>((value << 4U) | value);
}

uint8_t Expand5(uint32_t value) {
    return static_cast<uint8_t>((value << 3U) | (value >> 2U));
}

uint8_t Expand6(uint32_t value) {
    return static_cast<uint8_t>((value << 2U) | (value >> 4U));
}

uint32_t BytesPerPixel(uint32_t format) {
    switch (format) {
    case 0: return 4; // RGBA8
    case 1: return 3; // RGB8
    case 2: return 2; // RGB565
    case 3: return 2; // RGBA5551
    case 4: return 2; // RGBA4444
    default: return 0;
    }
}

Color DecodeColor(uint32_t format, const uint8_t* source) {
    const uint16_t packed = static_cast<uint16_t>(source[0]) |
                            (static_cast<uint16_t>(source[1]) << 8U);
    switch (format) {
    case 0: // RGBA8 (3DS memory order: [0]=A, [1]=B, [2]=G, [3]=R)
        return {source[3], source[2], source[1], source[0]};
    case 1: // RGB8 / BGR8_OES (3DS memory order: [0]=B, [1]=G, [2]=R)
        return {source[2], source[1], source[0], 0xFFU};
    case 2: // RGB565
        return {Expand5((packed >> 11U) & 0x1FU),
                Expand6((packed >> 5U) & 0x3FU),
                Expand5(packed & 0x1FU), 0xFFU};
    case 3: // RGBA5551
        return {Expand5((packed >> 11U) & 0x1FU),
                Expand5((packed >> 6U) & 0x1FU),
                Expand5((packed >> 1U) & 0x1FU),
                static_cast<uint8_t>((packed & 1U) != 0 ? 0xFFU : 0U)};
    case 4: // RGBA4444
        return {Expand4((packed >> 12U) & 0xFU),
                Expand4((packed >> 8U) & 0xFU),
                Expand4((packed >> 4U) & 0xFU), Expand4(packed & 0xFU)};
    default:
        return {};
    }
}

void EncodeColor(uint32_t format, const Color& color, uint8_t* target) {
    uint16_t packed = 0;
    switch (format) {
    case 0: // RGBA8 (3DS memory order: [0]=A, [1]=B, [2]=G, [3]=R)
        target[0] = color.A;
        target[1] = color.B;
        target[2] = color.G;
        target[3] = color.R;
        return;
    case 1: // RGB8 / BGR8_OES (3DS memory order: [0]=B, [1]=G, [2]=R)
        target[0] = color.B;
        target[1] = color.G;
        target[2] = color.R;
        return;
    case 2:
        packed = static_cast<uint16_t>((color.R >> 3U) << 11U) |
                 static_cast<uint16_t>((color.G >> 2U) << 5U) |
                 static_cast<uint16_t>(color.B >> 3U);
        break;
    case 3:
        packed = static_cast<uint16_t>((color.R >> 3U) << 11U) |
                 static_cast<uint16_t>((color.G >> 3U) << 6U) |
                 static_cast<uint16_t>((color.B >> 3U) << 1U) |
                 static_cast<uint16_t>(color.A >> 7U);
        break;
    case 4:
        packed = static_cast<uint16_t>((color.R >> 4U) << 12U) |
                 static_cast<uint16_t>((color.G >> 4U) << 8U) |
                 static_cast<uint16_t>((color.B >> 4U) << 4U) |
                 static_cast<uint16_t>(color.A >> 4U);
        break;
    default:
        return;
    }
    target[0] = static_cast<uint8_t>(packed);
    target[1] = static_cast<uint8_t>(packed >> 8U);
}

uint32_t MortonOffset(uint32_t x, uint32_t y, uint32_t bytesPerPixel) {
    constexpr std::array<uint32_t, 8> xLut{
        0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    constexpr std::array<uint32_t, 8> yLut{
        0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
    const uint32_t texel = xLut[x & 7U] + yLut[y & 7U] +
                           (x & ~7U) * 8U;
    return texel * bytesPerPixel;
}

Color Average(const Color& left, const Color& right) {
    return {static_cast<uint8_t>((uint16_t{left.R} + right.R) / 2U),
            static_cast<uint8_t>((uint16_t{left.G} + right.G) / 2U),
            static_cast<uint8_t>((uint16_t{left.B} + right.B) / 2U),
            static_cast<uint8_t>((uint16_t{left.A} + right.A) / 2U)};
}

} // namespace

bool ExecutePicaMemoryFill(const PicaMemoryFill& fill, HorizonOS* os, std::string* error) {
    if (fill.startAddress == 0U) {
        return true;
    }
    if (fill.endAddress <= fill.startAddress) {
        SetError(error, "PICA memory fill range is invalid");
        return false;
    }
    const size_t size = static_cast<size_t>(fill.endAddress - fill.startAddress);
    const size_t elementSize = (fill.control & (1U << 9U)) != 0U
                                   ? 4U
                                   : ((fill.control & (1U << 8U)) != 0U
                                          ? 3U
                                          : 2U);
    std::array<uint8_t, 4> value{};
    value[0] = static_cast<uint8_t>(fill.value & 0xFFU);
    value[1] = static_cast<uint8_t>((fill.value >> 8U) & 0xFFU);
    value[2] = static_cast<uint8_t>((fill.value >> 16U) & 0xFFU);
    value[3] = static_cast<uint8_t>((fill.value >> 24U) & 0xFFU);
    
    std::vector<uint8_t> bytes(size);
    for (size_t offset = 0; offset < bytes.size(); ++offset) {
        bytes[offset] = value[offset % elementSize];
    }
    return os->WriteBytes(fill.startAddress, bytes.data(), bytes.size());
}

bool ExecutePicaDisplayTransfer(const PicaDisplayTransfer& transfer, HorizonOS* os, std::string* error) {
    const uint32_t inputWidth = transfer.inputSize & 0xFFFFU;
    const uint32_t inputHeight = transfer.inputSize >> 16U;
    const uint32_t declaredOutputWidth = transfer.outputSize & 0xFFFFU;
    const uint32_t declaredOutputHeight = transfer.outputSize >> 16U;
    const bool flipVertically = (transfer.flags & 1U) != 0;
    const bool inputLinear = (transfer.flags & 2U) != 0;
    const bool cropInputLines = (transfer.flags & 4U) != 0;
    const bool textureCopy = (transfer.flags & 8U) != 0;
    const bool dontSwizzle = (transfer.flags & 0x20U) != 0;
    const uint32_t inputFormat = (transfer.flags >> 8U) & 7U;
    const uint32_t outputFormat = (transfer.flags >> 12U) & 7U;
    const bool block32 = (transfer.flags & 0x10000U) != 0;
    const uint32_t scaling = (transfer.flags >> 24U) & 3U;
    const uint32_t inputBpp = BytesPerPixel(inputFormat);
    const uint32_t outputBpp = BytesPerPixel(outputFormat);

    if (textureCopy || block32 || scaling > 2U || inputBpp == 0 ||
        outputBpp == 0 || inputWidth == 0 || inputHeight == 0 ||
        declaredOutputWidth == 0 || declaredOutputHeight == 0) {
        SetError(error, "unsupported or invalid PICA display transfer");
        return false;
    }
    if (inputLinear && scaling != 0U) {
        SetError(error, "PICA scaling requires tiled input");
        return false;
    }

    const uint32_t horizontalScale = scaling != 0U ? 1U : 0U;
    const uint32_t verticalScale = scaling == 2U ? 1U : 0U;
    const uint32_t outputWidth = declaredOutputWidth >> horizontalScale;
    const uint32_t outputHeight = declaredOutputHeight >> verticalScale;

    if (outputWidth == 0 || outputHeight == 0) {
        SetError(error, "scaled PICA display transfer has zero extent");
        return false;
    }

    const size_t inputByteCount = static_cast<size_t>(inputWidth) * inputHeight * inputBpp;
    const size_t outputByteCount = static_cast<size_t>(outputWidth) * outputHeight * outputBpp;

    uint32_t outputAddress = transfer.outputAddress;
    if (flipVertically && cropInputLines) {
        const int64_t adjustment = static_cast<int64_t>(inputWidth) - outputWidth;
        const int64_t byteAdjustment = adjustment * static_cast<int64_t>(outputHeight - 1U) * outputBpp;
        outputAddress += static_cast<uint32_t>(byteAdjustment);
    }

    std::vector<uint8_t> input(inputByteCount);
    std::vector<uint8_t> output(outputByteCount);

    if (!os->ReadBytes(transfer.inputAddress, input.data(), inputByteCount)) {
        SetError(error, "PICA display transfer memory read failed (input)");
        return false;
    }

#ifdef DEBUG_PICA_GPU
    uint32_t nonZeroIn = 0;
    for (size_t i = 0; i < input.size(); i += 8) {
        if (input[i] != 0) nonZeroIn++;
    }
    static int s_dtLog = 0;
    if (++s_dtLog % 60 == 1) {
        LOGI("DisplayTransfer: in=0x%08X out=0x%08X nonZeroIn=%u/%zu flags=0x%08X inSize=0x%08X outSize=0x%08X",
             transfer.inputAddress, transfer.outputAddress, nonZeroIn, input.size(),
             transfer.flags, transfer.inputSize, transfer.outputSize);
    }
#endif

    for (uint32_t y = 0; y < outputHeight; ++y) {
        for (uint32_t x = 0; x < outputWidth; ++x) {
            const uint32_t inputX = x << horizontalScale;
            const uint32_t inputY = y << verticalScale;
            const uint32_t outputY = flipVertically ? outputHeight - y - 1U : y;
            uint32_t sourceOffset = 0;
            uint32_t targetOffset = 0;
            if (inputLinear) {
                sourceOffset = (inputX + inputY * inputWidth) * inputBpp;
                targetOffset = dontSwizzle
                                   ? (x + outputY * outputWidth) * outputBpp
                                   : MortonOffset(x, outputY, outputBpp) +
                                         (outputY & ~7U) * outputWidth * outputBpp;
            } else {
                sourceOffset = MortonOffset(inputX, inputY, inputBpp) +
                               (inputY & ~7U) * inputWidth * inputBpp;
                targetOffset = dontSwizzle
                                   ? MortonOffset(x, outputY, outputBpp) +
                                         (outputY & ~7U) * outputWidth * outputBpp
                                   : (x + outputY * outputWidth) * outputBpp;
            }
            if (sourceOffset > input.size() - inputBpp ||
                targetOffset > output.size() - outputBpp) {
                SetError(error, "PICA display transfer pixel exceeds buffer");
                return false;
            }
            Color color = DecodeColor(inputFormat, input.data() + sourceOffset);
            if (scaling == 1U) {
                const uint32_t next = sourceOffset + inputBpp;
                if (next <= input.size() - inputBpp) {
                    color = Average(color, DecodeColor(inputFormat, input.data() + next));
                }
            } else if (scaling == 2U) {
                const uint32_t nextX = sourceOffset + inputBpp;
                const uint32_t nextY = sourceOffset + inputBpp * 2U;
                const uint32_t nextXY = sourceOffset + inputBpp * 3U;
                if (nextXY <= input.size() - inputBpp) {
                    color = Average(
                        Average(color, DecodeColor(inputFormat, input.data() + nextX)),
                        Average(DecodeColor(inputFormat, input.data() + nextY),
                                DecodeColor(inputFormat, input.data() + nextXY)));
                }
            }
            EncodeColor(outputFormat, color, output.data() + targetOffset);
        }
    }

#ifdef DEBUG_PICA_GPU
    uint32_t nonZeroOut = 0;
    for (size_t i = 0; i < output.size(); i += 8) {
        if (output[i] != 0) nonZeroOut++;
    }
    if (nonZeroOut > 50 || s_dtLog % 60 == 1) {
        LOGI("DisplayTransfer: wrote output to 0x%08X (%ux%u, fmt=%u, nonZeroOut=%u/%zu)",
             outputAddress, outputWidth, outputHeight, outputFormat, nonZeroOut, output.size());
    }
#endif

    if (!os->WriteBytes(outputAddress, output.data(), outputByteCount)) {
        SetError(error, "PICA display transfer memory write failed");
        return false;
    }

    // Notify OS that frame buffer at outputAddress was updated
    os->NotifyFramebufferUpdated(outputAddress, outputWidth, outputHeight, outputFormat);

    return true;
}
