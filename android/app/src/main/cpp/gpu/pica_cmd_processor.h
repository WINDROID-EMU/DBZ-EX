#ifndef PICA_CMD_PROCESSOR_H
#define PICA_CMD_PROCESSOR_H

#include <stdint.h>
#include <stddef.h>

class HorizonOS;

class PicaCommandProcessor {
public:
    PicaCommandProcessor();
    ~PicaCommandProcessor();

    void ProcessCommandList(const uint32_t* buffer, size_t wordCount, HorizonOS* os);

    uint32_t GetRegister(uint32_t regId) const;
    void SetRegister(uint32_t regId, uint32_t value, uint32_t mask);

private:
    uint32_t m_regs[0x400];
    uint32_t m_drawCallCount = 0;

    void ExecuteDrawArrays(HorizonOS* os);
    void ExecuteDrawElements(HorizonOS* os);
};

#endif // PICA_CMD_PROCESSOR_H
