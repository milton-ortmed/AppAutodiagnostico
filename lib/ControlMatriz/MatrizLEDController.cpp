#ifndef IS31_CONTROLLER_H
#define IS31_CONTROLLER_H

#include "ILightingController.h"
#include <Wire.h>

class IS31Matrix : public ILightingController {
private:
    uint8_t i2cAddress;
    uint8_t page_chip = 255; // Página actualmente seleccionada en el chip IS31
    uint8_t rows;
    uint8_t cols;
    uint8_t brightness = 255;
    uint32_t lastUpdateMs = 0;
    uint16_t amberSequenceIndex = 0;

    void selectPage(uint8_t page) {
        if (page_chip == page) return;

        Wire.beginTransmission(i2cAddress);
        Wire.write(0xFE); // Command Register
        Wire.write(0xC5); // Código de desbloqueo
        Wire.endTransmission();

        // Seleccionar la página
        Wire.beginTransmission(i2cAddress);
        Wire.write(0xFD); // Register de selección de página
        Wire.write(page);
        Wire.endTransmission();

        page_chip = page;
    }

    /**
    * @brief Escribe en un registro específico del chip IS31FL3733
    * 
    * @param page Número de la página a seleccionar
    * @param reg Dirección del registro a escribir
    * @param data Valor a escribir en el registro
    */
    void writeRegister(uint8_t page, uint8_t reg, uint8_t data) {
        selectPage(page);
        Wire.beginTransmission(i2cAddress);
        Wire.write(reg);
        Wire.write(data);
        Wire.endTransmission();
    }

public:
    static constexpr uint8_t DEFAULT_ROWS = 5;
    static constexpr uint8_t DEFAULT_COLS = 6;

    IS31Matrix(uint8_t i2cAddress, uint8_t rows = DEFAULT_ROWS, uint8_t cols = DEFAULT_COLS)
        : i2cAddress(i2cAddress), rows(rows), cols(cols) {}

    void begin() override {
        writeRegister(3, 0x00, 0x01);
        writeRegister(3, 0x01, brightness);

        selectPage(0);
        for (uint8_t i = 0; i < 0x18; i++) {
            Wire.beginTransmission(i2cAddress);
            Wire.write(i);
            Wire.write(0xFF);
            Wire.endTransmission();
        }
        selectPage(1);
        for (uint8_t i = 0; i < 0xC0; i++) {
            Wire.beginTransmission(i2cAddress);
            Wire.write(i);
            Wire.write(0x00);
            Wire.endTransmission();
        }
    }

    /**
    * @brief Set the Pixel object  
    * 
    * @param x columna (0-5) asociada a SW1-SW6
    * @param y fila (0-4) asociada a CS1-CS15
    * @param r profundidad de rojo (0-255)
    * @param g profundidad de verde (0-255)
    * @param b profundidad de azul (0-255)
    */
    void setPixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b) {
        if (x >= cols || y >= rows) return;

        uint8_t cs_r = y * 3 + 1;
        uint8_t cs_g = y * 3 + 0;
        uint8_t cs_b = y * 3 + 2;
        uint8_t base_addr = x * 16;

        // Escribir los colores en la Página 1 (Registros PWM)
        writeRegister(1, base_addr + cs_r, r);
        writeRegister(1, base_addr + cs_g, g);
        writeRegister(1, base_addr + cs_b, b);
    }

    /**
    * @brief Llena toda la matriz con un color específico
    * 
    * @param r profundidad de rojo (0-255)
    * @param g profundidad de verde (0-255)
    * @param b profundidad de azul (0-255)
    */
    void fillColor(uint8_t r, uint8_t g, uint8_t b) {
        for (uint8_t y = 0; y < rows; ++y) {
            for (uint8_t x = 0; x < cols; ++x) {
                setPixel(x, y, r, g, b);
            }
        }
    }

    void clear() {
        fillColor(0, 0, 0);
    }

    void setBrightness(uint8_t brightness) override {
        this->brightness = brightness;
        writeRegister(3, 0x01, brightness);
    }

    /**
     * @brief Efecto IDLE: Respiración tenue con sutil titileo cálido ("Hada viviendo dentro").
     * Totalmente asíncrono y guiado por millis().
     */
    void updateIdleEffect() override {
        uint32_t now = millis();
        if (now - lastUpdateMs < 20) return; // Limitar actualización a ~50 FPS
        lastUpdateMs = now;

        uint8_t pulse = (uint8_t)((sin((float)now / 500.0f) * 0.5f + 0.5f) * 200.0f + random(40, 55)); // Oscilación suave entre 0 y 255 con un toque de aleatoriedad
        for (uint8_t y = 0; y < rows; ++y) {
            for (uint8_t x = 0; x < cols; ++x) {
                setPixel(x, y, pulse, pulse / 3, 0);
            }
        }
    }

    void updateShowEffect() override {
        uint32_t now = millis();
        if (now - lastUpdateMs < 15) return;
        lastUpdateMs = now;

        uint8_t nowCicle = now % 255;
        uint8_t pulse = (uint8_t)(255.0f - (float)nowCicle/24.0f); // Decaimiento suave de 255 a 0 en aproximadamente 6 segundos

        if (random(0, 100) < 5) { // 5% de probabilidad de cambiar el color base
            posx = random(0, cols);
            posy = random(0, rows);
        }

        for (uint8_t y = 0; y < rows; ++y) {
            for (uint8_t x = 0; x < cols; ++x) {
                if (x == posx && y == posy) {
                    setPixel(x, y, 255, 255, 255); // Pixel central blanco brillante
                } else {
                    setPixel(x, y, pulse, pulse / 2, pulse); // Otros píxeles con decaimiento suave
                }
            }
        }
    }

    void updateAmberSequenceEffect() override {
        uint32_t now = millis();
        if (now - lastUpdateMs < 100) return;
        lastUpdateMs = now;

        fillColor(0, 0, 0);
        uint8_t index = amberSequenceIndex;
        uint8_t x = index % cols;
        uint8_t y = index / cols;
        setPixel(x, y, 255, 180, 0);
        amberSequenceIndex = (amberSequenceIndex + 1) % (rows * cols);
    }

    void updateAmberSequenceEffect2() override {
        uint32_t now = millis();
        if (now - lastUpdateMs < 100) return;
        lastUpdateMs = now;

        fillColor(0, 0, 0);
        uint8_t index = amberSequenceIndex;
        uint8_t x = index % cols;
        uint8_t y = index / cols;
        setPixel(x, y, 255 - (x * 20), 180 + (y * 10), 255);
        amberSequenceIndex = (amberSequenceIndex + 1) % (rows * cols);
    }
};

#endif // IS31_CONTROLLER_H