#ifndef I_LIGHTING_CONTROLLER_H
#define I_LIGHTING_CONTROLLER_H

#include <Arduino.h>

class ILightingController {
public:
    virtual ~ILightingController() {}

    /**
     * @brief Inicializa los pines y periféricos de la tira LED.
     */
    virtual void begin() = 0;

    /**
     * @brief Actualiza la animación de estado en reposo (IDLE).
     * Efecto tenue parpadeante/respiración ("Fairy Glow"). No bloqueante.
     */
    virtual void updateIdleEffect() = 0;

    /**
     * @brief Actualiza la animación durante el espectáculo (SHOW_RUNNING).
     * Efecto mágico brillante con destellos. No bloqueante.
     */
    virtual void updateShowEffect() = 0;

    /**
     * @brief Actualiza la secuencia de recorrido ámbar durante el espectáculo.
     */
    virtual void updateAmberSequenceEffect() = 0;

    /**
     * @brief Actualiza una secuencia de recorrido con colores del efecto SHOW.
     */
    virtual void updateAmberSequenceEffect2() = 0;

    /**
     * @brief Define el nivel de brillo general de los LEDs.
     * @param brightness Valor entre 0 y 255.
     */
    virtual void setBrightness(uint8_t brightness) = 0;
};

#endif // I_LIGHTING_CONTROLLER_H