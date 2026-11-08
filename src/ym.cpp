// The YM2151 through ymfm, as the X16 emulator's ymglue.cpp drives it
#include "ym.h"

#include "ymfm_opm.h"

namespace {

class Interface : public ymfm::ymfm_interface {
public:
    Interface() : chip(*this) {}

    void ymfm_sync_mode_write(uint8_t data) override { m_engine->engine_mode_write(data); }
    void ymfm_sync_check_interrupts() override { m_engine->engine_check_interrupts(); }

    ymfm::ym2151 chip;
};

Interface *opm;

}  // namespace

extern "C" {

void ym_reset(void)
{
    if (!opm)
        opm = new Interface();
    opm->chip.reset();
}

void ym_write(uint8_t reg, uint8_t val)
{
    // The game's writes are paced by its music tick, far slower than the
    // chip's busy time, so they go straight in
    opm->chip.write_address(reg);
    opm->chip.write_data(val);
}

void ym_generate(int16_t lr[2])
{
    ymfm::ym2151::output_data out;
    opm->chip.generate(&out);
    for (int c = 0; c < 2; c++) {
        int32_t v = out.data[c];
        lr[c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
}

}
