// AnalogTester - Herramienta de Diagnóstico PRISMA para RP2040
// Conecta tu pedal de expresión y abre el Serial Monitor (Ctrl+Shift+M en Arduino IDE)
// Asegúrate de que los baudios en el monitor estén a 115200.

// Incluir librería obligatoria para usar el puerto Serial USB en RP2040 (Adafruit TinyUSB stack)
#include <Adafruit_TinyUSB.h>

const int PIN_PEDAL_1 = A0; 
const int PIN_PEDAL_2 = A1; 

void setup() {
  // Inicializamos el puerto Serie para comunicarnos con la PC
  Serial.begin(115200);
  
  // Damos un pequeño respiro para que el puerto se abra
  delay(2000);
  
  Serial.println("--- PRISMA Analog Tester INICIADO ---");
  Serial.println("Mueve los pedales para ver cómo cambia la señal en crudo.");
  Serial.println("Rango esperado: 0 a 1023 (o cerca).");
  Serial.println("=========================================");
}

void loop() {
  // Leemos los valores puros directos de los procesadores analógicos
  int val1 = analogRead(PIN_PEDAL_1);
  int val2 = analogRead(PIN_PEDAL_2);
  
  // Imprimimos el resultado de una forma cómoda de leer
  Serial.print("Pedal 1 (A0): ");
  Serial.print(val1);
  Serial.print("\t|\tPedal 2 (A1): ");
  Serial.println(val2);

  // Pausa de 100 milisegundos para no saturar el monitor
  delay(100);
}
