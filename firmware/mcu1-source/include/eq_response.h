#ifndef OMNI_EQ_RESPONSE_H
#define OMNI_EQ_RESPONSE_H
/* RBJ/BLT preview at an assumed 48 kHz processing rate, not DSP readback.
 * Type 1 peak,2 low-pass,3 high-pass,4 low-shelf,5 high-shelf.
 * Returns signed hundredths of a dB; unsupported/disabled bands contribute zero. */
int omni_eq_response_band(unsigned hz,unsigned gain,unsigned q,unsigned type,unsigned probe_hz);
unsigned omni_eq_response_frequency(unsigned column);
#endif
