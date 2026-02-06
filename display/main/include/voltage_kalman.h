#ifndef VOLTAGE_KALMAN_H
#define VOLTAGE_KALMAN_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
   VoltageToPercentage with Kalman filtering
   ------------------------------------------------------------
   This module converts measured battery voltage (in mV)
   into an estimated battery percentage (0–100 %).
   It also provides a simple 1D Kalman filter for smoothing
   noisy ADC readings before conversion.
   ------------------------------------------------------------ */

/* ----------------------------
   1D Kalman filter structure
   ---------------------------- */
typedef struct {
    double x;      // estimated voltage (V)
    double P;      // estimation error covariance
    double Q;      // process noise variance
    double R;      // measurement noise variance
    int initialized;
} Kalman1D;

/* ------------------------------------------------------------
   Function prototypes
   ------------------------------------------------------------ */

/**
 * @brief Initialize the Kalman filter.
 *
 * @param kf       Pointer to Kalman1D structure.
 * @param init_x   Initial voltage estimate (in Volts).
 * @param init_P   Initial error covariance.
 * @param Q        Process noise variance (smaller = smoother).
 * @param R        Measurement noise variance.
 */
void kalman_init(Kalman1D *kf, double init_x, double init_P, double Q, double R);

/**
 * @brief Update the Kalman filter with a new voltage measurement.
 *
 * @param kf   Pointer to Kalman1D structure.
 * @param z    New measured voltage (in Volts).
 * @return     Filtered voltage (in Volts).
 */
double kalman_update(Kalman1D *kf, double z);

/**
 * @brief Convert a raw voltage (in mV) to percentage (0–100 %)
 *        using a two-segment linear regression model.
 *
 * @param voltage_mv   Measured voltage in millivolts (e.g., 3750 = 3.750 V).
 * @return             Battery percentage (0–100 %).
 */
double voltage_to_percentage_mv(int voltage_mv);

/**
 * @brief Apply Kalman filter and then convert to percentage.
 *
 * @param kf        Pointer to Kalman1D structure.
 * @param raw_mv    Raw measured voltage in millivolts.
 * @return          Smoothed battery percentage (0–100 %).
 */
double VoltageToPercentage_WithKalman(Kalman1D *kf, int raw_mv);

#ifdef __cplusplus
}
#endif

#endif /* VOLTAGE_KALMAN_H */

