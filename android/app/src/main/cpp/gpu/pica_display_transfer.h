#ifndef PICA_DISPLAY_TRANSFER_H
#define PICA_DISPLAY_TRANSFER_H

#include <stdint.h>
#include <stddef.h>
#include <string>

struct PicaDisplayTransfer {
    uint32_t inputAddress = 0;
    uint32_t outputAddress = 0;
    uint32_t outputSize = 0; // (height << 16) | width
    uint32_t inputSize = 0;  // (height << 16) | width
    uint32_t flags = 0;
};

struct PicaMemoryFill {
    uint32_t startAddress = 0;
    uint32_t endAddress = 0;
    uint32_t value = 0;
    uint32_t control = 0;
};

class HorizonOS;

bool ExecutePicaDisplayTransfer(const PicaDisplayTransfer& transfer, HorizonOS* os, std::string* error = nullptr);
bool ExecutePicaMemoryFill(const PicaMemoryFill& fill, HorizonOS* os, std::string* error = nullptr);

#endif // PICA_DISPLAY_TRANSFER_H
