#include <LiquidCrystal.h>
#include <Keypad.h>
#include <DHT.h>
#include "AsyncTaskLib.h"
#include "StateMachineLib.h"
#include <SPI.h>
#include <MFRC522.h>

// ----------------- DEFINICIONES DE HARDWARE -----------------
#define DHTPIN 22
#define DHTTYPE DHT11
#define LED_ROJO A5
#define LED_VERDE A4
#define LED_AZUL A3
#define BUZZER_PIN 7
#define VENTILADOR_PIN A7
#define LUZ_PIN A1
#define RST_PIN 32
#define SS_PIN 53

// ----------------- OBJETOS -----------------
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal lcd(12, 11, 5, 4, 3, 2);
MFRC522 mfrc522(SS_PIN, RST_PIN);
float pmv = 0.0;

// Teclado
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {30, 31, 34, 36};
byte colPins[COLS] = {38, 40, 42, 44};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ----------------- VARIABLES DE SISTEMA -----------------
char contrasenia[5] = "1234";  // Contraseña del sistema
char ingreso[5];               // Buffer para ingresar contraseña
int indice = 0;                // Índice para entrada de contraseña
int intentos = 0;              // Intentos fallidos de contraseña
bool accesoPermitido = false;  // Estado de acceso

float temperatura = 0;
float humedad = 0;
int luz = 0;
int alarmasConsecutivas = 0;

// UIDs de las tarjetas RFID
byte tarjetaAltoUID[4] = {0x62, 0xEC, 0xA9, 0x00};   // Card UID: E6 C5 D4 38 (Confort Alto)62 EC A9 00
byte tarjetaBajoUID[4] = {0x43, 0x68, 0xAB, 0xA1};   // Card UID: 43 68 AB A1 (Confort Bajo)

// ----------------- ENUMERACIONES -----------------
enum State {
  INIT = 0,
  BLOQUEADO,
  MONITOREO,
  ALARMA,
  CONF_TERM_ALTO,
  CONF_TERM_BAJO
};

enum Input {
  TIMEOUT = 0,
  TEMP_LUZ_ALTA,
  TECLA_ASTERISCO,
  BLOQUEO,
  ACCESO_PERMITIDO,
  INTENTOS_AGOTADOS,
  PMV_ALTO,
  PMV_BAJO,
  Unknown
};

Input input = Unknown;
StateMachine stateMachine(6, 10);

// ----------------- FUNCIONES AUXILIARES -----------------
bool compararUID(byte *uid1, byte *uid2, byte size) {
  for (byte i = 0; i < size; i++) {
    if (uid1[i] != uid2[i]) {
      return false;
    }
  }
  return true;
}

bool leerTarjetaRFID() {
  // Verificar si hay una tarjeta nueva presente
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return false;
  }
  
  // Leer el UID de la tarjeta
  if (!mfrc522.PICC_ReadCardSerial()) {
    return false;
  }
  
  // Mostrar UID en el monitor serial para depuración
  Serial.print("UID de la tarjeta: ");
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
  }
  Serial.println();
  
  // Comparar con la tarjeta de confort alto
  if (compararUID(mfrc522.uid.uidByte, tarjetaAltoUID, 4)) {
    Serial.println("Tarjeta de confort ALTO detectada");
    input = PMV_ALTO;
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return true;
  }
  
  // Comparar con la tarjeta de confort bajo
  if (compararUID(mfrc522.uid.uidByte, tarjetaBajoUID, 4)) {
    Serial.println("Tarjeta de confort BAJO detectada");
    input = PMV_BAJO;
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return true;
  }
  
  // Si no coincide con ninguna tarjeta conocida
  Serial.println("Tarjeta desconocida detectada");
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  return false;
}
// ----------------- TAREAS ASÍNCRONAS -----------------
AsyncTask Task_LED_Correcto(1000, false, []() {
  digitalWrite(LED_VERDE, HIGH);
  delay(1000);
  digitalWrite(LED_VERDE, LOW);
});

AsyncTask Task_LED_Incorrecto(1000, false, []() {
  digitalWrite(LED_AZUL, HIGH);
  delay(1000);
  digitalWrite(LED_AZUL, LOW);
});

AsyncTask Task_MonitoreoAmbiente(2000, true, []() {
  temperatura = dht.readTemperature();
  humedad = dht.readHumidity();
  luz = analogRead(LUZ_PIN);

  Serial.print("T: "); Serial.print(temperatura);
  Serial.print(" H: "); Serial.print(humedad);
  Serial.print(" L: "); Serial.println(luz);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("T:"); lcd.print(temperatura);
  lcd.print("C H:"); lcd.print(humedad);
  lcd.print("%");
  lcd.setCursor(0, 1);
  lcd.print("L:"); lcd.print(luz);

  // Verificar tarjetas RFID (para cambio de estado directo)
  leerTarjetaRFID();

  if (temperatura > 40 || luz < 10) {
    input = TEMP_LUZ_ALTA;
  }
});

AsyncTask Task_Timeout_Alarma(3000, false, []() { 
  input = TIMEOUT; 
});

bool ledAlarma = false;
AsyncTask Task_LED_Alarma(800, true, []() {
  digitalWrite(LED_ROJO, ledAlarma ? LOW : HIGH);
  ledAlarma = !ledAlarma;
  Task_LED_Alarma.SetIntervalMillis(ledAlarma ? 800 : 200);

  if (alarmasConsecutivas >= 3) {
    input = INTENTOS_AGOTADOS;
  }
});

bool ledBloqueo = false;
AsyncTask Task_LED_Bloqueo(200, true, []() {
  digitalWrite(LED_ROJO, ledBloqueo ? LOW : HIGH);
  ledBloqueo = !ledBloqueo;
  Task_LED_Bloqueo.SetIntervalMillis(ledBloqueo ? 200 : 100);
  
  char key = keypad.getKey();
  if(key == '*'){
    input = TECLA_ASTERISCO;
  }
});

bool buzzerState = false;
AsyncTask Task_Buzzer(800, true, []() {
  digitalWrite(BUZZER_PIN, buzzerState ? LOW : HIGH);
  buzzerState = !buzzerState;
  Task_Buzzer.SetIntervalMillis(buzzerState ? 800 : 200);
});

AsyncTask Task_Timeout_ConfTermAlto(5000, false, []() {
  digitalWrite(VENTILADOR_PIN, LOW);
  input = TIMEOUT;
});

AsyncTask Task_Timeout_ConfTermBajo(5000, false, []() {
  digitalWrite(LED_VERDE, LOW);
  input = TIMEOUT;
});

// Función para leer la contraseña del teclado
void leerClave() {
  char key = keypad.getKey();
  if (key && indice < 4) {
    ingreso[indice] = key;
    lcd.setCursor(indice, 1);
    lcd.print('*');
    indice++;
  }

  if (indice == 4) {
    ingreso[4] = '\0'; // Agregar terminador nulo
    if (strcmp(contrasenia, ingreso) == 0) {
      accesoPermitido = true;
      lcd.clear();
      lcd.print("ACCESO CORRECTO");
      Task_LED_Correcto.Start();
      input = ACCESO_PERMITIDO;
      indice = 0;
      memset(ingreso, 0, sizeof(ingreso));
    } else {
      intentos++;
      lcd.clear();
      lcd.print("CLAVE INCORRECTA");
      lcd.setCursor(0, 1);
      lcd.print("Intento ");
      lcd.print(intentos);
      lcd.print("/3");
      Task_LED_Incorrecto.Start();
      
      if (intentos >= 3) {
        input = INTENTOS_AGOTADOS;
      } else {
        // Restablecer para nuevo intento
        delay(2000);
        lcd.clear();
        lcd.print("DIGITE SU CLAVE:");
        indice = 0;
        memset(ingreso, 0, sizeof(ingreso));
      }
    }
  }
}

// ----------------- FUNCIONES DE ESTADO -----------------
void outputINIT() {
  Serial.println("Estado: INIT - Verificando clave de seguridad");
  lcd.clear();
  lcd.print("DIGITE SU CLAVE:");
  accesoPermitido = false;
  intentos = 0;
  indice = 0;
  memset(ingreso, 0, sizeof(ingreso));
}

void outputBLOQUEADO() {
  Serial.println("Estado: BLOQUEADO - Sistema bloqueado");
  alarmasConsecutivas = 0;
  lcd.clear(); 
  lcd.print("SISTEMA BLOQUEADO");
  lcd.setCursor(0, 1);
  lcd.print("Presione *");
  Task_LED_Bloqueo.Start();
}

void outputMONITOREO() {
  Serial.println("Estado: MONITOREO - Monitoreo ambiental");
  lcd.clear();
  lcd.print("Modo Monitoreo");
  Task_MonitoreoAmbiente.Start();
}

void outputALARMA() {
  Serial.println("Estado: ALARMA - Condiciones criticas");
  lcd.clear(); 
  lcd.print("ALARMA ACTIVADA");
  Task_Buzzer.Start();
  Task_LED_Alarma.Start();
  Task_Timeout_Alarma.Start();
  alarmasConsecutivas++;
}

void outputCONF_TERM_ALTO() {
  Serial.println("Estado: CONF_TERM_ALTO - Confort termico alto");
  lcd.clear(); 
  lcd.print("Confort termico");
  lcd.setCursor(0, 1);
  lcd.print("ALTO - Ventilador");
  digitalWrite(VENTILADOR_PIN, HIGH);
  Task_Timeout_ConfTermAlto.Start();
}

void outputCONF_TERM_BAJO() {
  Serial.println("Estado: CONF_TERM_BAJO - Confort termico bajo");
  lcd.clear(); 
  lcd.print("Confort termico");
  lcd.setCursor(0, 1);
  lcd.print("BAJO - LED Verde");
  digitalWrite(LED_VERDE, HIGH);
  Task_Timeout_ConfTermBajo.Start();
}

// ----------------- SETUP Y LOOP -----------------
void setupStateMachine() {
  // Transiciones de estado
  stateMachine.AddTransition(INIT, BLOQUEADO, []() { return input == INTENTOS_AGOTADOS; });
  stateMachine.AddTransition(BLOQUEADO, INIT, []() { return input == TECLA_ASTERISCO; });
  stateMachine.AddTransition(INIT, MONITOREO, []() { return input == ACCESO_PERMITIDO; });
  stateMachine.AddTransition(MONITOREO, ALARMA, []() { return input == TEMP_LUZ_ALTA; });
  stateMachine.AddTransition(ALARMA, MONITOREO, []() { return input == TIMEOUT; });
  stateMachine.AddTransition(ALARMA, BLOQUEADO, []() { return input == INTENTOS_AGOTADOS; });
  stateMachine.AddTransition(MONITOREO, CONF_TERM_ALTO, []() { return input == PMV_ALTO; });
  stateMachine.AddTransition(CONF_TERM_ALTO, MONITOREO, []() { return input == TIMEOUT; });
  stateMachine.AddTransition(MONITOREO, CONF_TERM_BAJO, []() { return input == PMV_BAJO; });
  stateMachine.AddTransition(CONF_TERM_BAJO, MONITOREO, []() { return input == TIMEOUT; });

  // Funciones de entrada a estados
  stateMachine.SetOnEntering(INIT, outputINIT);
  stateMachine.SetOnEntering(BLOQUEADO, outputBLOQUEADO);
  stateMachine.SetOnEntering(MONITOREO, outputMONITOREO);
  stateMachine.SetOnEntering(ALARMA, outputALARMA);
  stateMachine.SetOnEntering(CONF_TERM_ALTO, outputCONF_TERM_ALTO);
  stateMachine.SetOnEntering(CONF_TERM_BAJO, outputCONF_TERM_BAJO);

  // Funciones de salida de estados
  stateMachine.SetOnLeaving(BLOQUEADO, []() { 
    Task_LED_Bloqueo.Stop(); 
    digitalWrite(LED_ROJO, LOW);
  });
  stateMachine.SetOnLeaving(ALARMA, []() {
    Task_LED_Alarma.Stop();
    Task_Buzzer.Stop();
    Task_Timeout_Alarma.Stop();
    digitalWrite(LED_ROJO, LOW);
    digitalWrite(BUZZER_PIN, LOW);
  });
  stateMachine.SetOnLeaving(MONITOREO, []() { 
    Task_MonitoreoAmbiente.Stop();
  });
  stateMachine.SetOnLeaving(CONF_TERM_ALTO, []() { 
    Task_Timeout_ConfTermAlto.Stop();
  });
  stateMachine.SetOnLeaving(CONF_TERM_BAJO, []() { 
    Task_Timeout_ConfTermBajo.Stop();
  });
}

void setup() {
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.begin(9600);
  lcd.begin(16, 2);
  dht.begin();

  // Configurar pines
  pinMode(LED_ROJO, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AZUL, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(VENTILADOR_PIN, OUTPUT);
  pinMode(LUZ_PIN, INPUT);

  // Asegurar que todo empiece apagado
  digitalWrite(LED_ROJO, LOW);
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_AZUL, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(VENTILADOR_PIN, LOW);
  
  setupStateMachine();
  stateMachine.SetState(INIT, false, true);
}

void loop() {
  // Actualizar todas las tareas activas
  if (Task_LED_Correcto.IsActive()) Task_LED_Correcto.Update();
  if (Task_LED_Incorrecto.IsActive()) Task_LED_Incorrecto.Update();
  if (Task_MonitoreoAmbiente.IsActive()) Task_MonitoreoAmbiente.Update();
  if (Task_LED_Bloqueo.IsActive()) Task_LED_Bloqueo.Update();
  if (Task_LED_Alarma.IsActive()) Task_LED_Alarma.Update();
  if (Task_Buzzer.IsActive()) Task_Buzzer.Update();
  if (Task_Timeout_Alarma.IsActive()) Task_Timeout_Alarma.Update();
  if (Task_Timeout_ConfTermAlto.IsActive()) Task_Timeout_ConfTermAlto.Update();
  if (Task_Timeout_ConfTermBajo.IsActive()) Task_Timeout_ConfTermBajo.Update();

  // Procesar la máquina de estados
  stateMachine.Update();
  
  // Si estamos en estado INIT, leer la contraseña
  if (stateMachine.GetState() == INIT) {
    leerClave();
  }
  
  // Solo resetear la entrada si no hay una entrada pendiente
  if (input != Unknown && stateMachine.GetState() != INIT) {
    input = Unknown;
  }
}