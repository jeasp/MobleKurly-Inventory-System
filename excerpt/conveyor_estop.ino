// 컨베이어 구동, 비상정지, 상태 LED (MobleKurly_Arduino.ino 중 일부)

// 핀 설정 (컨베이어 PUL/DIR, RGB LED, 비상정지 버튼)
const int PUL_PIN = 7;
const int DIR_PIN = 8;
// ...
const int RGB_RED_PIN = 9;
const int RGB_GREEN_PIN = 10;
const int RGB_BLUE_PIN = 11;
// ...
const bool RGB_COMMON_ANODE = false; // 공통 GND=false, 공통 +=true
// ...
// [ESTOP] 실물 버튼: D24-GND, INPUT_PULLUP, 눌림=HIGH
const int ESTOP_PIN = 24;

// 디바운스·방향·펄스 간격
const unsigned long RELEASE_DEBOUNCE_MS = 30;

const int CONVEYOR_DIRECTION = LOW; // 반대 방향은 LOW
const unsigned long PULSE_DELAY_US = 140;

// 펄스·비상정지 상태 변수
bool pulseState = HIGH;

// [ESTOP] 정지 후에는 새로운 실행 명령을 받아야 재가동합니다.
bool conveyorRunning = true; // 기존 전원 ON 자동 실행 유지
bool physicalPressed = false;
bool releasePending = false;
unsigned long releaseStartedAt = 0;
unsigned long stopRevision = 0;
char startBuffer[16];
int startLength = 0;
bool readingStart = false;
bool discardStart = false;
unsigned long startReceivedAt = 0;

// 가동(초록)/정지(빨강) LED
void updateStatusLed()
{
    int running = conveyorRunning ? 1 : 0;
    if (lastLedRunning == running) return;
    lastLedRunning = running;
    int on = RGB_COMMON_ANODE ? LOW : HIGH;
    int off = RGB_COMMON_ANODE ? HIGH : LOW;
    digitalWrite(RGB_RED_PIN, running ? off : on);
    digitalWrite(RGB_GREEN_PIN, running ? on : off);
    digitalWrite(RGB_BLUE_PIN, off);
}

// 상태 보고, 정지, 비상정지 버튼, 정지번호 확인 후 재가동
void printConveyorState()
{
    Serial.print("STATE,");
    Serial.print(conveyorRunning ? 1 : 0);
    Serial.print(",");
    Serial.print(physicalPressed ? 1 : 0);
    Serial.print(",");
    Serial.println(stopRevision);
}

void stopConveyor()
{
    Serial.println("DBG,CONVEYOR,STOP_REQUEST");
    conveyorRunning = false; // PUL 현재 레벨 유지: 추가 펄스를 만들지 않음
    stopRevision++;
    printConveyorState();
}

void updateEmergencyButton()
{
    if (digitalRead(ESTOP_PIN) == HIGH)
    {
        releasePending = false;
        if (!physicalPressed)
        {
            physicalPressed = true;
            Serial.println("DBG,ESTOP,D24_HIGH_BUTTON_PRESSED");
            stopConveyor();
        }
    }
    else if (physicalPressed)
    {
        if (!releasePending)
        {
            releasePending = true;
            releaseStartedAt = millis();
        }
        else if (millis() - releaseStartedAt >= RELEASE_DEBOUNCE_MS)
        {
            physicalPressed = false;
            releasePending = false;
            Serial.println("DBG,ESTOP,BUTTON_RELEASED_RESTART_REQUIRED");
            printConveyorState(); // 해제만으로는 실행하지 않음
        }
    }
}

void startConveyorFromBuffer()
{
    unsigned long revision = 0;
    bool valid = startLength > 1 && startBuffer[0] == ',';
    for (int i = 1; valid && i < startLength; i++)
    {
        char c = startBuffer[i];
        if (c < '0' || c > '9' || revision > (4294967295UL - (c - '0')) / 10UL)
            valid = false;
        else
            revision = revision * 10UL + (c - '0');
    }
    if (!valid) { Serial.println("DBG,CONVEYOR,START_BAD_FORMAT"); Serial.println("ERR,BAD_START"); }
    else if (physicalPressed || digitalRead(ESTOP_PIN) == HIGH)
    {
        Serial.println("DBG,CONVEYOR,START_REJECTED_BUTTON_PRESSED");
        Serial.println("ERR,BUTTON_PRESSED");
    }
    else if (revision != stopRevision)
    {
        Serial.println("DBG,CONVEYOR,START_REJECTED_STALE_REVISION");
        Serial.println("ERR,STALE_START");
    }
    else
    {
        lastPulse = micros();
        conveyorRunning = true;
        Serial.println("DBG,CONVEYOR,START_ACCEPTED");
    }
    printConveyorState();
}

// 비상정지·컨베이어·LED 초기화
    pinMode(ESTOP_PIN, INPUT_PULLUP);
    physicalPressed = digitalRead(ESTOP_PIN) == HIGH;
    if (physicalPressed)
    {
        conveyorRunning = false; // 실물 버튼이 눌린 경우에만 시작 차단
        stopRevision++;
    }


    digitalWrite(PUL_PIN, HIGH);
    pinMode(PUL_PIN, OUTPUT);

    digitalWrite(DIR_PIN, CONVEYOR_DIRECTION);
    pinMode(DIR_PIN, OUTPUT);

    // [ALERT] 실제 컨베이어 상태로 초기 LED를 설정합니다.
    int ledOff = RGB_COMMON_ANODE ? HIGH : LOW;
    digitalWrite(RGB_RED_PIN, ledOff);
    digitalWrite(RGB_GREEN_PIN, ledOff);
    digitalWrite(RGB_BLUE_PIN, ledOff);
    pinMode(RGB_RED_PIN, OUTPUT);
    pinMode(RGB_GREEN_PIN, OUTPUT);
    pinMode(RGB_BLUE_PIN, OUTPUT);
    // ...
    lastLedRunning = -1;
    updateStatusLed();

// 버튼 우선 확인, micros()로 PUL 펄스 출력
void loop()
{
    // [ESTOP] 통신/출고 상태와 관계없이 버튼을 먼저 확인
    updateEmergencyButton();
    // ...
    updateStatusLed();
    // ...
    // 1. 실행 상태일 때 기존 속도로 컨베이어 실행
    unsigned long nowMicros = micros();

    if (conveyorRunning && nowMicros - lastPulse >= PULSE_DELAY_US)
    {
        lastPulse = nowMicros;
        pulseState = !pulseState;
        digitalWrite(PUL_PIN, pulseState);
        ++stepTransitions;
    }

// 잘린 실행 명령 버림
    // [ESTOP] 잘린 실행 명령을 숫자 서보 명령으로 해석하지 않음
    if (readingStart && millis() - startReceivedAt > 250)
    {
        readingStart = false;
        discardStart = true;
    }

// 시리얼 명령 ! 정지 / ? 상태 / R,<정지번호> 실행
    // 3. 시리얼 명령 처리
    if (Serial.available() > 0)
    {
        char key = Serial.read();

        // [ESTOP] !: 즉시 정지 / ?: 상태 / R,<정지번호>\n: 실행
        if (key == '!')
        {
            Serial.println("DBG,SERIAL,SOFTWARE_ESTOP_EXCLAMATION");
            discardStart = discardStart || readingStart;
            readingStart = false;
            stopConveyor();
            return;
        }
        if (key == '?')
        {
            Serial.println("DBG,SERIAL,STATUS_QUERY");
            printConveyorState();
            return;
        }
        if (discardStart)
        {
            if (key == '\n') discardStart = false;
            return;
        }
        if (readingStart)
        {
            if (key == '\n')
            {
                readingStart = false;
                startConveyorFromBuffer();
            }
            else if (key != '\r')
            {
                if (startLength < (int)sizeof(startBuffer))
                    startBuffer[startLength++] = key;
                else
                {
                    readingStart = false;
                    discardStart = true;
                    Serial.println("ERR,BAD_START");
                }
            }
            return;
        }
        if (key == 'R')
        {
            Serial.println("DBG,SERIAL,START_COMMAND_RECEIVED");
            readingStart = true;
            startLength = 0;
            startReceivedAt = millis();
            return;
        }

        // ...
    }
}
