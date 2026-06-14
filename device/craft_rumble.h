/*
 * ThumbyElite — rumble motor driver (Thumby Color, GP5 / PWM2B).
 */
#ifndef CRAFT_RUMBLE_H
#define CRAFT_RUMBLE_H

void craft_rumble_init(void);
/* Pulse at intensity 0..1 for seconds (overrides any running pulse if
 * stronger or longer). */
void craft_rumble_pulse(float intensity, float seconds);
/* Call once per frame to time-out pulses. */
void craft_rumble_tick(float dt);

#endif
