#include "calibration.h"
#include "../sensors/as7265x_driver.h"
#include <Arduino.h>
#include <string.h>

// ─── Physical model: counts → transmittance → absorbance ────────────────────
//
// The AS7265X reports an integer "count" per channel that is proportional to
// the photon flux hitting that channel's photodiode during one integration
// window.  Counts have no SI unit on their own — they depend on the gain and
// integration time the sensor was configured with.  To turn counts into a
// physically meaningful quantity we follow the standard absorption-spectroscopy
// procedure built around the Beer–Lambert law:
//
//     A(λ) = log10( I0(λ) / I(λ) ) = ε(λ) · c · ℓ
//
// where
//   I0(λ) = light intensity reaching the detector with the BLANK in the
//           cuvette path (e.g. distilled water for an aqueous assay).  This
//           is what `Calibration::reference[i]` stores after averaging
//           CALIBRATION_AVERAGES (5) one-shot reads.
//   I(λ)  = light intensity reaching the detector with the SAMPLE in the
//           cuvette path.  This is what `MeasurementEngine` records into
//           `Experiment::spectra[m][i]` for measurement m, channel i.
//   T(λ)  = I(λ) / I0(λ)  ∈ [0, 1]  — transmittance (dimensionless ratio,
//           usually shown as a percentage  T_pct = T · 100 ).
//   A(λ)  = -log10(T)               — absorbance (dimensionless).
//   ε     = molar absorptivity, c = concentration, ℓ = path length.  These
//           three are what an analytical method calibrates against; we only
//           need accurate T to feed downstream models.
//
// Step-by-step validation of the chart Y axis:
//   1. Sensor returns raw counts.   Units: counts          (gain & int-time
//                                          dependent — meaningless across runs)
//   2. We separately measure I0 with the blank.            counts
//   3. Per channel we compute  Δ = I - I0 .                counts (signed)
//      Δ is allowed to be NEGATIVE — sample absorbs more than blank.
//   4. The viewer reconstructs  I = Δ + I0  and then       counts
//      T = I / I0 = (Δ + I0) / I0           dimensionless (0..~1)
//      T_pct = T · 100                       percent
//   5. (Optional) absorbance  A = -log10(T)  dimensionless
//
// Why we keep the SUBTRACTED form on disk instead of raw counts:
//   • Backward-compatibility with the existing CSV / DB schema that has been
//     populated with `Δ` in the channel columns and `I0` in the cal_* columns.
//   • The viewer can recover I (and T) losslessly as long as we do NOT clamp
//     Δ at zero.  The previous version of this file clamped Δ ≥ 0 which
//     destroyed transmittance information for any absorbing sample (the
//     normal case in spectroscopy: I < I0 → Δ < 0).  That clamp is removed
//     below — Δ is now signed.
// ────────────────────────────────────────────────────────────────────────────

Calibration g_calibration;

// Minimum gap between samples.  takeMeasurements() in Mode 3 blocks for
// ~420 ms (50 cycles × 2.8 ms × 3 dies).  Add 500 ms on top so the sensor
// registers have time to settle before the next one-shot trigger.
static const unsigned long CAL_SAMPLE_INTERVAL_MS = 500;

Calibration::Calibration()
    : _running(false), _done(false), _failed(false),
      _samplesCollected(0), _samplesTarget(CALIBRATION_AVERAGES), _lastSampleTime(0) {
    memset(&_data, 0, sizeof(_data));
    memset(_accumulator, 0, sizeof(_accumulator));
}

void Calibration::start(uint8_t n_target) {
    _running = true;
    _done    = false;
    _failed  = false;
    _samplesCollected = 0;
    _samplesTarget = (n_target > 0) ? n_target : CALIBRATION_AVERAGES;
    if (_samplesTarget < 1) _samplesTarget = 1;
    _lastSampleTime   = 0;   // force immediate first sample
    memset(_accumulator, 0, sizeof(_accumulator));
    memset(&_data, 0, sizeof(_data));
    Serial.printf("[Cal] Calibration started (N=%d)\n", _samplesTarget);
}

int Calibration::samplesCollected() const { return _samplesCollected; }
int Calibration::samplesTarget()    const { return _samplesTarget; }

void Calibration::tick() {
    if (!_running || _done) return;

    unsigned long now = millis();
    if (now - _lastSampleTime < CAL_SAMPLE_INTERVAL_MS) return;
    _lastSampleTime = now;

    float reading[NUM_CHANNELS];
    if (!g_sensorDriver.takeMeasurement(reading)) {
        _failed = true;
        _running = false;
        Serial.println("[Cal] Sensor read failed");
        return;
    }

    for (int i = 0; i < NUM_CHANNELS; i++) {
        _accumulator[i] += reading[i];
    }
    _samplesCollected++;
    Serial.printf("[Cal] Sample %d/%d collected\n", _samplesCollected, _samplesTarget);

    if (_samplesCollected >= _samplesTarget) {
        for (int i = 0; i < NUM_CHANNELS; i++) {
            _data.reference[i] = _accumulator[i] / (float)_samplesTarget;
            _data.offset[i]    = _data.reference[i];
        }
        _data.valid      = true;
        _data.cfg_at_cal = g_sensorDriver.getConfig();   // freeze config (R2)
        _data.n_used     = (uint8_t)_samplesTarget;
        _done    = true;
        _running = false;
        Serial.println("[Cal] Calibration complete");
    }
}

bool Calibration::isDone() const { return _done; }
bool Calibration::hasFailed() const { return _failed; }
void Calibration::clearDoneFlag() { _done = false; }

const CalibrationData& Calibration::getData() const { return _data; }

void Calibration::reset() {
    _running = false;
    _done    = false;
    _failed  = false;
    _samplesCollected = 0;
    memset(&_data, 0, sizeof(_data));
    memset(_accumulator, 0, sizeof(_accumulator));
}

void Calibration::applyOffset(float* inOut18) const {
    if (!_data.valid) return;
    for (int i = 0; i < NUM_CHANNELS; i++) {
        // Δ = I_sample - I_blank.  SIGNED — do not clamp at 0.
        // Negative Δ means the sample absorbs/scatters more light than the
        // blank reference, which is the normal case for a colored solution.
        // Clamping at 0 destroys transmittance: the viewer reconstructs
        // I = Δ + I_blank → T = I / I_blank, and a clamped Δ collapses
        // every absorbing channel to T = 100 %.
        inOut18[i] -= _data.offset[i];
    }
}
