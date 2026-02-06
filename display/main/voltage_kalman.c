#include "voltage_kalman.h"
#include <math.h>

/* ------------------------------------------------------------
   Simple 1D Kalman Filter implementation
   ------------------------------------------------------------ */

/**
 * @brief Initialize the Kalman filter parameters.
 */
void kalman_init(Kalman1D *kf, double init_x, double init_P, double Q, double R)
{
    kf->x = init_x;
    kf->P = init_P;
    kf->Q = Q;
    kf->R = R;
    kf->initialized = 1;
}

/**
 * @brief Perform one update step of the Kalman filter.
 *
 * The model assumes a constant voltage process (no dynamics).
 */
double kalman_update(Kalman1D *kf, double z)
{
    if (!kf->initialized) {
        kalman_init(kf, z, 1.0, 1e-5, 1e-2);
    }

    /* Prediction step */
    double x_prior = kf->x;
    double P_prior = kf->P + kf->Q;

    /* Update step */
    double K = P_prior / (P_prior + kf->R);  // Kalman gain
    double x_post = x_prior + K * (z - x_prior);
    double P_post = (1.0 - K) * P_prior;

    kf->x = x_post;
    kf->P = P_post;

    return kf->x;
}

/* ------------------------------------------------------------
   Two-segment linear regression model
   ------------------------------------------------------------ */

/* Split voltage (Volts) */
#define SPLIT_VOLTAGE  3.75

/* Regression coefficients (for percentage in 0..1 range) */
#define LOW_SLOPE      (-2.7028640535916564)
#define LOW_INTERCEPT  (10.785042745141201)
#define HIGH_SLOPE     (-1.429001608144687)
#define HIGH_INTERCEPT (5.8990585961092075)

/**
 * @brief Convert voltage in millivolts to battery percentage (0–100 %).
 *
 * The conversion uses two linear models separated by SPLIT_VOLTAGE.
 * The result is clamped to [0, 100].
 */
double voltage_to_percentage_mv(int voltage_mv)
{
    double v = voltage_mv / 1000.0;  // Convert mV to Volts
    double frac;

    if (v <= SPLIT_VOLTAGE)
        frac = LOW_SLOPE * v + LOW_INTERCEPT;
    else
        frac = HIGH_SLOPE * v + HIGH_INTERCEPT;

    // Clamp to 0–1
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    // Invert the result: higher voltage should give higher percentage
    frac = 1.0 - frac;

    return frac * 100.0;
}

/**
 * @brief Apply Kalman filtering to raw voltage (in mV)
 *        and return smoothed battery percentage (0–100 %).
 */
double VoltageToPercentage_WithKalman(Kalman1D *kf, int raw_mv)
{
    double filtered_v = kalman_update(kf, raw_mv / 1000.0);
    return voltage_to_percentage_mv((int)(filtered_v * 1000.0));
}

