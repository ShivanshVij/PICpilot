/* 
 * File:   AttitudeManager.c
 * Author: Mitch
 *
 * Created on June 15, 2013, 3:40 PM
 */
 
//Include Header Files
#include "delay.h"
#include "VN100.h"
#include "InputCapture.h"
#include "OutputCompare.h"
#include "PWM.h"
#include "AttitudeManager.h"
#include "commands.h"
#include "cameraManager.h"
#include "Probe_Drop.h"
#include "StartupErrorCodes.h"
#include "main.h"
#include "InterchipDMA.h"


extern PMData pmData;
extern AMData amData;
extern char DMADataAvailable;

long int lastTime = 0;
long int heartbeatTimer = 0;
long int UHFSafetyTimer = 0;
long int gpsTimer = 0;

float* velocityComponents;

// Setpoints (From radio transmitter or autopilot)
int sp_PitchRate = 0;
int sp_ThrottleRate = MIN_PWM;
int sp_FlapRate = MIN_PWM;
int sp_YawRate = 0;
int sp_RollRate = 0;

int tail_OutputR;   //what the rudder used to be
int tail_OutputL;


int sp_ComputedPitchRate = 0;
//int sp_ComputedThrottleRate = 0;
int sp_ComputedRollRate = 0;
int sp_ComputedYawRate = 0;

char currentGain = 0;

int sp_PitchAngle = 0;
int ctrl_PitchAngle = 0;
//float sp_YawAngle = 0;
int sp_RollAngle = 0;
int ctrl_RollAngle = 0;

//Heading Variables
int sp_Heading = 0;
int ctrl_Heading = 0;

//Altitude Variables
int sp_Altitude = 0;
int ctrl_Altitude = 0;
float sp_GroundSpeed = 0;

//GPS Data
int gps_Heading = 0;
float gps_GroundSpeed = 0; //NOTE: NEEDS TO BE IN METERS/SECOND. CALCULATIONS DEPEND ON THESE UNITS. GPS RETURNS KM/H.
float gps_Time = 0;
long double gps_Longitude = 0;
long double gps_Latitude = 0;
float gps_Altitude = 0;
float airspeed = 0;
char gps_Satellites = 0;
char gps_PositionFix = 0;
char waypointIndex = 0;
char waypointChecksum = 0;
char waypointCount = 0;
char batteryLevel1 = 0;


// System outputs (get from IMU)
float imuData[3];
float imu_RollRate = 0;
float imu_PitchRate = 0;
float imu_YawRate = 0;

//IMU integration outputs
float imu_RollAngle = 0;
float imu_PitchAngle = 0;
float imu_YawAngle = 0;

int rollTrim = 0;
int pitchTrim = 0;
int yawTrim = 0;

//RC Input Signals (Input Capture Values)
int input_RC_RollRate = 0;
int input_RC_PitchRate = 0;
int input_RC_Throttle = 0;
int input_RC_Flap = 0;
int input_RC_YawRate = 0;
int input_RC_Aux1 = 0; //0=Roll, 1= Pitch, 2=Yaw
int input_RC_Aux2 = 0; //0 = Saved Value, 1 = Edit Mode
int input_RC_Switch1 = 0;
int input_RC_UHFSwitch = 0;

//Ground Station Input Signals
int input_GS_Roll = 0;
int input_GS_Pitch = 0;
int input_GS_Throttle = 0;
int input_GS_Flap = 0;
int input_GS_Yaw = 0;
int input_GS_RollRate = 0;
int input_GS_PitchRate = 0;
int input_GS_YawRate = 0;
int input_GS_Altitude = 0;

int input_AP_Altitude = 0;

//PID Global Variable Storage Values
int rollPID, pitchPID, throttlePID, yawPID, flapPID;

float scaleFactor = 20; //Change this

char displayGain = 0;
int controlLevel = 0;
int lastCommandSentCode = 0;

int headingCounter = 0;
char altitudeTrigger = 0;

float refRotationMatrix[9];
float lastAltitude = 0;
long int lastAltitudeTime = 0;

char lastNumSatellites = 0;

unsigned int cameraCounter = 0;

void attitudeInit() {
    //Initialize Timer
    initTimer4();

    //Initialize Interchip communication
    TRISFbits.TRISF3 = 0;
    LATFbits.LATF3 = 1;

    TRISDbits.TRISD14 = 0;
    LATDbits.LATD14 = 0;

    amData.checkbyteDMA = generateAMDataDMAChecksum();

    //Initialize Interchip Interrupts for Use in DMA Reset
    //Set opposite Input / Output Configuration on the PathManager
    TRISAbits.TRISA12 = 0;  //Init RA12 as Output (0), (1) is Input
    INTERCOM_1 = 0;    //Set RA12 to Output a Value of 0
    TRISAbits.TRISA13 = 0;  //Init RA13 as Output (0), (1) is Input
    INTERCOM_2 = 0;    //Set RA13 to Output a Value of 0

    TRISBbits.TRISB4 = 1;   //Init RB4 as Input (1), (0) is Output
    TRISBbits.TRISB5 = 1;   //Init RB5 as Input (1), (0) is Output
    TRISAbits.TRISA3 = 0;
    PORTAbits.RA3 = 1;

    init_SPI1();
    init_DMA0();
    init_DMA1();


    /* Initialize Input Capture and Output Compare Modules */
#if DEBUG
    initPWM(0b10011111, 0b11111111);
    debug("INITIALIZATION - ATTITUDE MANAGER");
#else
    initPWM(0b10011111, 0b11111111);
#endif
    /* Initialize IMU with correct orientation matrix and filter settings */
    //In order: Angular Walk, Angular Rate x 3, Magnetometer x 3, Acceleration x 3
    float filterVariance[10] = {1e-9, 1e-9, 1e-9, 1e-9, 1, 1, 1, 1e-3, 1e-3, 1e-3}; //  float filterVariance[10] = {1e-10, 1e-6, 1e-6, 1e-6, 1e-2, 1e-2, 1e-2, 1e-2, 1e-2, 1e-2};
    VN100_initSPI();
    //IMU position matrix
    // offset = {roll, pitch, yaw}
    float cal_roll = -90;
    float cal_pitch = -90;
    float cal_yaw = 0.0;
    float offset[3] = {cal_roll,cal_pitch,cal_yaw};
    setVNOrientationMatrix((float*)&offset);
    VN100_SPI_SetFiltMeasVar(0, (float*)&filterVariance);
    initialization();
}


char checkDMA(){
    //Transfer data from PATHMANAGER CHIP
    lastNumSatellites = gps_Satellites; //get the last number of satellites
    DMADataAvailable = 0;

    if (generatePMDataDMAChecksum() == pmData.checkbyteDMA) {
        gps_Time = pmData.time;
        input_AP_Altitude = pmData.sp_Altitude;
        gps_Satellites = pmData.satellites;
        gps_PositionFix = pmData.positionFix;
        waypointIndex = pmData.targetWaypoint;
        batteryLevel1 = pmData.batteryLevel;
        waypointCount = pmData.waypointCount;
        airspeed = pmData.airspeed;

        //Check if this data is new and requires action or if it is old and redundant
        if (gps_Altitude == pmData.altitude && gps_Heading == pmData.heading && gps_GroundSpeed == pmData.speed && gps_Latitude == pmData.latitude && gps_Longitude == pmData.longitude){
            return FALSE;
        }
        gps_Heading = pmData.heading;
        gps_GroundSpeed = pmData.speed * 1000.0/3600.0; //Convert from km/h to m/s
        gps_Longitude = pmData.longitude;
        gps_Latitude = pmData.latitude;
        gps_Altitude = pmData.altitude;
        if (gps_PositionFix){
            sp_Heading = pmData.sp_Heading;
        }
    }
    return TRUE;
}

float getAltitude(){
    return gps_Altitude;
}
int getHeading(){
    return gps_Heading;
}
long double getLongitude(){
    return gps_Longitude;
}
long double getLatitude(){
    return gps_Latitude;
}
float getRoll(){
    return imu_RollAngle;
}
float getPitch(){
    return imu_PitchAngle;
}
float getYaw(){
    return imu_YawAngle;
}
float getRollRate(){
    return imu_RollRate;
}
float getPitchRate(){
    return imu_PitchRate;
}
float getYawRate(){
    return imu_YawRate;
}
int getRollAngleSetpoint(){
    return sp_RollAngle;
}
int getPitchAngleSetpoint(){
    return sp_PitchAngle;
}
int getPitchRateSetpoint(){
    return sp_PitchRate;
}
int getRollRateSetpoint(){
    return sp_RollRate;
}
int getYawRateSetpoint(){
    return sp_YawRate;
}
int getThrottleSetpoint(){
    return sp_ThrottleRate;
}
int getFlapSetpoint(){
    return sp_FlapRate;
}
int getAltitudeSetpoint(){
    return sp_Altitude;
}
int getHeadingSetpoint(){
    return sp_Heading;
}

void setPitchAngleSetpoint(int setpoint){
    sp_PitchAngle = setpoint;
}
void setRollAngleSetpoint(int setpoint){
    sp_RollAngle = setpoint;
}
void setPitchRateSetpoint(int setpoint){
    sp_PitchRate = setpoint;
}
void setRollRateSetpoint(int setpoint){
    sp_RollRate = setpoint;
}
void setYawRateSetpoint(int setpoint){
    sp_YawRate = setpoint;
}
void setThrottleSetpoint(int setpoint){
    sp_ThrottleRate = setpoint;
}
void setFlapSetpoint(int setpoint){
    sp_FlapRate = setpoint;
}
void setAltitudeSetpoint(int setpoint){
    sp_Altitude = setpoint;
}
void setHeadingSetpoint(int setpoint){
    sp_Heading = setpoint;
}

void inputCapture(){
    int* channelIn;
    channelIn = getPWMArray();
    inputMixing(channelIn, &input_RC_RollRate, &input_RC_PitchRate, &input_RC_Throttle, &input_RC_YawRate, &input_RC_Flap);

    // Switches and Knobs
    input_RC_UHFSwitch = channelIn[4];
//        sp_Type = channelIn[5];
//        sp_Value = channelIn[6];
    input_RC_Switch1 = channelIn[7];

    //Controller Input Interpretation Code
    if (input_RC_Switch1 > MIN_PWM && input_RC_Switch1 < MIN_PWM + 50) {
        unfreezeIntegral();
    } else {
        freezeIntegral();
    }
}

int getPitchAngleInput(char source){
    if (source == PITCH_RC_SOURCE){
        return (int)((input_RC_PitchRate / ((float)SP_RANGE / MAX_PITCH_ANGLE) ));
    }
    else if (source == PITCH_GS_SOURCE){
        return input_GS_Pitch;
    }
    else
        return 0;
}
int getPitchRateInput(char source){
    if (source == PITCH_RC_SOURCE){
        return input_RC_PitchRate;
    }
    else if (source == PITCH_GS_SOURCE){
        return input_GS_PitchRate;
    }
    else
        return 0;
}
int getRollAngleInput(char source){
    if (source == ROLL_RC_SOURCE){
        return (int)((input_RC_RollRate / ((float)SP_RANGE / MAX_ROLL_ANGLE) ));
    }
    else if (source == ROLL_GS_SOURCE){
        return input_GS_Roll;
    }
    else{
        return 0;
    }
}
int getRollRateInput(char source){
    if (source == ROLL_RC_SOURCE){
        return input_RC_RollRate;
    }
    else if (source == ROLL_GS_SOURCE){
        return input_GS_RollRate;
    }
    else
        return 0;
}
int getThrottleInput(char source){
    if (source == THROTTLE_RC_SOURCE){
        return input_RC_Throttle;
    }
    else if (source == THROTTLE_GS_SOURCE){
        return input_GS_Throttle;
    }
    else if (source == THROTTLE_AP_SOURCE){
//        return input_AP_Throttle;
        return 0;
    }
    else
        return 0;
}

int getFlapInput(char source){
    if (source == FLAP_RC_SOURCE){
        return input_RC_Flap;
    }
    else if (source == FLAP_GS_SOURCE){
        return input_GS_Flap;
    }
    else if (source == FLAP_AP_SOURCE){
//        return input_AP_Flap;
        return 0;
    }
    else
        return 0;
}

int getAltitudeInput(char source){
    if (source == ALTITUDE_GS_SOURCE){
        return input_GS_Altitude;
    }
    else if (source == ALTITUDE_AP_SOURCE){
        return input_AP_Altitude;
    }
    else
        return 0;
}

void setKValues(int type,float* values){
    int Kchannel[7] = {YAW, PITCH, ROLL, HEADING, ALTITUDE, THROTTLE, FLAP};
    int i;
    for(i=0; i<7; i++){
       setGain(Kchannel[i],type,values[i]);
    }
};

void imuCommunication(){
    /*****************************************************************************
     *****************************************************************************
                                IMU COMMUNICATION
     *****************************************************************************
     *****************************************************************************/
    VN100_SPI_GetRates(0, (float*) &imuData);

    //TODO: This is a reminder for me to figure out a more elegant way to fix improper derivative control (based on configuration of the sensor), adding this negative is a temporary fix. Some kind of calibration command or something.
    //DO NOT ADD NEGATIVES IN THE STATEMENTS BELOW. IT IS A GOOD WAY TO ROYALLY SCREW YOURSELF OVER LATER.
    //Outputs in order: Roll,Pitch,Yaw
    imu_RollRate = (imuData[IMU_ROLL_RATE]);
    imu_PitchRate = imuData[IMU_PITCH_RATE];
    imu_YawRate = imuData[IMU_YAW_RATE];
    VN100_SPI_GetYPR(0, &imuData[YAW], &imuData[PITCH], &imuData[ROLL]);
    imu_YawAngle = imuData[YAW];
    imu_PitchAngle = imuData[PITCH];
    imu_RollAngle = (imuData[ROLL]);
#if DEBUG
    // Rate - Radians, Angle - Degrees
//    char x[30];
//    sprintf(&x, "IMU Roll Rate: %f", imu_RollRate);
//    debug(&x);
//    sprintf(&x, "IMU Pitch Rate: %f", imu_PitchRate);
//    debug(&x);
//    sprintf(&x, "IMU Pitch Angle: %f", imu_PitchAngle);
//    debug(&x);
//    sprintf(&x, "IMU Roll Angle: %f", imu_RollAngle);
//    debug(&x);
#endif
}

int altitudeControl(int setpoint, int sensorAltitude){
    //Altitude
    ctrl_PitchAngle = controlSignalAltitude(setpoint, sensorAltitude);
    if (ctrl_PitchAngle > MAX_PITCH_ANGLE)
        ctrl_PitchAngle = MAX_PITCH_ANGLE;
    if (ctrl_PitchAngle < -MAX_PITCH_ANGLE)
        ctrl_PitchAngle = -MAX_PITCH_ANGLE;
    return ctrl_PitchAngle;
}

int throttleControl(int setpoint, int sensor){
    //Throttle
    throttlePID = sp_ThrottleRate + controlSignalThrottle(setpoint, sensor);      
    return throttlePID;
}

int flapControl(int setpoint, int sensor){
    //Flaps
    flapPID = sp_FlapRate + controlSignalFlap(setpoint, sensor);      
    return flapPID;
}

//Equivalent to "Yaw Angle Control"
int headingControl(int setpoint, int sensor){
    //Heading
    while (setpoint > 360)
        setpoint -= 360;
    while (setpoint < 0)
        setpoint += 360;

    setHeadingSetpoint(setpoint);
    ctrl_Heading = controlSignalHeading(setpoint, sensor);//gps_Satellites>=4?gps_Heading:(int)imu_YawAngle); //changed to monitor satellites, since we know these are good values while PositionFix might be corrupt...
    //Approximating Roll angle from Heading
    sp_RollAngle = ctrl_Heading;      //TODO: HOW IS HEADING HANDLED DIFFERENTLY BETWEEN QUADS AND PLANES

    if (sp_RollAngle > MAX_ROLL_ANGLE)
        sp_RollAngle = MAX_ROLL_ANGLE;
    if (sp_RollAngle < -MAX_ROLL_ANGLE)
        sp_RollAngle = -MAX_ROLL_ANGLE;
    return sp_RollAngle;
}


int rollAngleControl(int setpoint, int sensor){
    //Roll Angle
    sp_ComputedRollRate = controlSignalAngles(setpoint, sensor, ROLL, -(SP_RANGE) / (MAX_ROLL_ANGLE));
    return sp_ComputedRollRate;
}

int pitchAngleControl(int setpoint, int sensor){
    //Pitch Angle
    sp_ComputedPitchRate = controlSignalAngles(setpoint, sensor, PITCH, -(SP_RANGE) / (MAX_PITCH_ANGLE)); //Removed negative
    return sp_ComputedPitchRate;
}

int coordinatedTurn(float pitchRate, int rollAngle){
    //Feed forward Term when turning
    pitchRate += (int)(scaleFactor * abs(rollAngle)); //Linear Function
    return pitchRate;
}

int rollRateControl(float setpoint, float sensor){
    rollPID = controlSignal(setpoint/SERVO_SCALE_FACTOR, sensor, ROLL);
    return rollPID;
}
int pitchRateControl(float setpoint, float sensor){
    pitchPID = controlSignal(setpoint/SERVO_SCALE_FACTOR, sensor, PITCH);
    return pitchPID;
}
int yawRateControl(float setpoint, float sensor){
    yawPID = controlSignal(setpoint/SERVO_SCALE_FACTOR, sensor, YAW);
    return yawPID;
}

char getControlPermission(unsigned int controlMask, unsigned int expectedValue, char bitshift){
    int maskResult = (controlMask & controlLevel);
    return (maskResult >> bitshift) == expectedValue;
}

void readDatalink(void){
  
    struct command* cmd = popCommand();
    //TODO: Add rudimentary input validation
    if ( cmd ) {
        if (lastCommandSentCode/100 == cmd->cmd){
            lastCommandSentCode++;
        }
        else{
            lastCommandSentCode = cmd->cmd * 100;
        }
        switch (cmd->cmd) {
            case DEBUG_TEST:             // Debugging command, writes to debug UART
#if DEBUG
                debug("Foo");
                debug( (char*) cmd->data);
#endif
                break;
            case SET_PITCH_KD_GAIN:
                setGain(PITCH, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_ROLL_KD_GAIN:
                setGain(ROLL, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_YAW_KD_GAIN:
                setGain(YAW, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_PITCH_KP_GAIN:
                setGain(PITCH, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_ROLL_KP_GAIN:
                setGain(ROLL, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_YAW_KP_GAIN:
                setGain(YAW, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_PITCH_KI_GAIN:
                setGain(PITCH, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_ROLL_KI_GAIN:
                setGain(ROLL, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_YAW_KI_GAIN:
                setGain(YAW, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_HEADING_KD_GAIN:
                setGain(HEADING, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_HEADING_KP_GAIN:   
                setGain(HEADING, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_HEADING_KI_GAIN:
                setGain(HEADING, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_ALTITUDE_KD_GAIN:
                setGain(ALTITUDE, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_ALTITUDE_KP_GAIN:
                setGain(ALTITUDE, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_ALTITUDE_KI_GAIN:
                setGain(ALTITUDE, GAIN_KI, *(float*)(&cmd->data));
                break;
            case SET_THROTTLE_KD_GAIN:
                setGain(THROTTLE, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_THROTTLE_KP_GAIN:
                setGain(THROTTLE, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_THROTTLE_KI_GAIN:
                setGain(THROTTLE, GAIN_KI, *(float*)(&cmd->data));
                break;
                
            case SET_FLAP_KD_GAIN:
                setGain(FLAP, GAIN_KD, *(float*)(&cmd->data));
                break;
            case SET_FLAP_KP_GAIN:
                setGain(FLAP, GAIN_KP, *(float*)(&cmd->data));
                break;
            case SET_FLAP_KI_GAIN:
                setGain(FLAP, GAIN_KI, *(float*)(&cmd->data));
                break;    
            
            case SET_PATH_GAIN:
                amData.pathGain = *(float*)(&cmd->data);
                amData.command = PM_SET_PATH_GAIN;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SET_ORBIT_GAIN:
                amData.orbitGain = *(float*)(&cmd->data);
                amData.command = PM_SET_ORBIT_GAIN;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SHOW_GAIN:
                displayGain = *(char*)(&cmd->data);
                break;
            case SET_PITCH_RATE:
                input_GS_PitchRate = *(int*)(&cmd->data);
                break;
            case SET_ROLL_RATE:
                input_GS_RollRate = *(int*)(&cmd->data);
                break;
            case SET_YAW_RATE:
                input_GS_YawRate = *(int*)(&cmd->data);
                break;
            case SET_PITCH_ANGLE:
                input_GS_Pitch = *(int*)(&cmd->data);
                break;
            case SET_ROLL_ANGLE:
                input_GS_Roll = *(int*)(&cmd->data);
                break;
            case SET_YAW_ANGLE:
//                sp_YawAngle = *(int*)(&cmd->data);
                break;
            case SET_ALTITUDE:
                input_GS_Altitude = *(int*)(&cmd->data);
                break;
            case SET_HEADING:
                sp_Heading = *(int*)(&cmd->data);
                break;
            case SET_THROTTLE:
                input_GS_Throttle = (int)(((long int)(*(int*)(&cmd->data))) * MAX_PWM * 2 / 100) - MAX_PWM;
                break;
            case SET_FLAP:
                input_GS_Flap = (int)(((long int)(*(int*)(&cmd->data))) * MAX_PWM * 2 / 100) - MAX_PWM;
                break;
            case SET_AUTONOMOUS_LEVEL:
                controlLevel = *(int*)(&cmd->data);
                break;
            case SET_ANGULAR_WALK_VARIANCE:
                setAngularWalkVariance(*(float*)(&cmd->data));
                break;
            case SET_GYRO_VARIANCE:
                setGyroVariance(*(float*)(&cmd->data));
                break;
            case SET_MAGNETIC_VARIANCE:
                setMagneticVariance(*(float*)(&cmd->data));
                break;
            case SET_ACCEL_VARIANCE:
                setAccelVariance(*(float*)(&cmd->data));
                break;
            case SET_SCALE_FACTOR:
                scaleFactor = *(float*)(&cmd->data);
                break;
            case CALIBRATE_ALTIMETER:
                amData.calibrationHeight = *(float*)(&cmd->data);
                amData.command = PM_CALIBRATE_ALTIMETER;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case CLEAR_WAYPOINTS:
                amData.waypoint.id = (*(char *)(&cmd->data)); //Dummy Data
                amData.command = PM_CLEAR_WAYPOINTS;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case REMOVE_WAYPOINT:
                amData.waypoint.id = (*(char *)(&cmd->data));
                amData.command = PM_REMOVE_WAYPOINT;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SET_TARGET_WAYPOINT:
                amData.waypoint.id = *(char *)(&cmd->data);
                amData.command = PM_SET_TARGET_WAYPOINT;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case RETURN_HOME:
                amData.command = PM_RETURN_HOME;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case CANCEL_RETURN_HOME:
                amData.command = PM_CANCEL_RETURN_HOME;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SEND_HEARTBEAT:
                heartbeatTimer = getTime();
                break;
            case TRIGGER_CAMERA:
                triggerCamera(*(unsigned int*)(&cmd->data));
                break;
            case SET_TRIGGER_DISTANCE:
                setTriggerDistance(*(float*)(&cmd->data));
                break;
            case SET_GIMBLE_OFFSET:
                setGimbalOffset(*(unsigned int*)(&cmd->data));
                break;
            case KILL_PLANE:
                if (*(int*)(&cmd->data) == 1234)
//                    killingPlane = 1;
                break;
            case UNKILL_PLANE:
                if (*(int*)(&cmd->data) == 1234)
//                    killingPlane = 0;
                break;
            case LOCK_GOPRO:
                    lockGoPro(*(int*)(&cmd->data));
                break;
            case ARM_VEHICLE:
                if (*(int*)(&cmd->data) == 1234)
                    startArm();
                break;
            case DEARM_VEHICLE:
                if (*(int*)(&cmd->data) == 1234)
                    stopArm();
                break;
            case DROP_PROBE:
                dropProbe(*(char*)(&cmd->data));
                break;
            case RESET_PROBE:
                //dropProbe(*(char*)(&cmd->data);
                break;

            case NEW_WAYPOINT:
                amData.waypoint.altitude = (*(WaypointWrapper*)(&cmd->data)).altitude;
                amData.waypoint.id = (*(WaypointWrapper*)(&cmd->data)).id;
                amData.waypoint.latitude = (*(WaypointWrapper*)(&cmd->data)).latitude;
                amData.waypoint.longitude = (*(WaypointWrapper*)(&cmd->data)).longitude;
                amData.waypoint.radius = (*(WaypointWrapper*)(&cmd->data)).radius;
                amData.command = PM_NEW_WAYPOINT;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case INSERT_WAYPOINT:
                amData.waypoint.altitude = (*(WaypointWrapper*)(&cmd->data)).altitude;
                amData.waypoint.latitude = (*(WaypointWrapper*)(&cmd->data)).latitude;
                amData.waypoint.longitude = (*(WaypointWrapper*)(&cmd->data)).longitude;
                amData.waypoint.radius = (*(WaypointWrapper*)(&cmd->data)).radius;
                amData.waypoint.nextId = (*(WaypointWrapper*)(&cmd->data)).nextId;
                amData.waypoint.previousId = (*(WaypointWrapper*)(&cmd->data)).previousId;
                amData.command = PM_INSERT_WAYPOINT;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case SET_RETURN_HOME_COORDINATES:
                amData.waypoint.altitude = (*(WaypointWrapper*)(&cmd->data)).altitude;
                amData.waypoint.latitude = (*(WaypointWrapper*)(&cmd->data)).latitude;
                amData.waypoint.longitude = (*(WaypointWrapper*)(&cmd->data)).longitude;
                amData.command = PM_SET_RETURN_HOME_COORDINATES;
                amData.checkbyteDMA = generateAMDataDMAChecksum();
                amData.checksum = generateAMDataChecksum(&amData);
                break;
            case TARE_IMU:
                adjustVNOrientationMatrix((float*)(&cmd->data));
                break;
            case SET_IMU:
                setVNOrientationMatrix((float*)(&cmd->data));
                break;
            case SET_KDVALUES:
                setKValues(GAIN_KD,(float*)(&cmd->data));
                break;
            case SET_KPVALUES:
                setKValues(GAIN_KP,(float*)(&cmd->data));
                break;
            case SET_KIVALUES:
                setKValues(GAIN_KI,(float*)(&cmd->data));
                break;
            default:
                break;
        }
        destroyCommand( cmd );
    }
 
}
int writeDatalink(p_priority packet){
     
    struct telem_block* statusData = createTelemetryBlock(packet);//getDebugTelemetryBlock();

    switch(packet){
        case PRIORITY1:
            statusData->data.p1_block.lat = getLatitude();
            statusData->data.p1_block.lon = getLongitude();
            statusData->data.p1_block.sysTime = getTime();
            statusData->data.p1_block.roll = getRoll();
            statusData->data.p1_block.pitch = getPitch();
            statusData->data.p1_block.yaw = getYaw();
            statusData->data.p1_block.rollRate = getRollRate();
            statusData->data.p1_block.pitchRate = getPitchRate();
            statusData->data.p1_block.yawRate = getYawRate();
            statusData->data.p1_block.airspeed = airspeed;
            statusData->data.p1_block.alt = getAltitude();
            statusData->data.p1_block.UTC = gps_Time;
            statusData->data.p1_block.gSpeed = gps_GroundSpeed;
            statusData->data.p1_block.heading = getHeading();
            break;
        case PRIORITY2:
            statusData->data.p2_block.lastCommandSent = lastCommandSentCode;
            statusData->data.p2_block.batteryLevel1 = batteryLevel1;
            //statusData->data.p2_block.batteryLevel2 =
            statusData->data.p2_block.startupErrorCodes = getStartupErrorCodes();
            int* input = getPWMArray();
            statusData->data.p2_block.ch1In = input[0];
            statusData->data.p2_block.ch2In = input[1];
            statusData->data.p2_block.ch3In = input[2];
            statusData->data.p2_block.ch4In = input[3];
            statusData->data.p2_block.ch5In = input[4];
            statusData->data.p2_block.ch6In = input[5];
            statusData->data.p2_block.ch7In = input[6];
            statusData->data.p2_block.ch8In = input[7];
            int* output = checkPWMArray();
            statusData->data.p2_block.ch1Out = output[0];
            statusData->data.p2_block.ch2Out = output[1];
            statusData->data.p2_block.ch3Out = output[2];
            statusData->data.p2_block.ch4Out = output[3];
            statusData->data.p2_block.ch5Out = output[4];
            statusData->data.p2_block.ch6Out = output[5];
            statusData->data.p2_block.ch7Out = output[6];
            statusData->data.p2_block.ch8Out = output[7];
            statusData->data.p2_block.rollRateSetpoint = getRollRateSetpoint();
            statusData->data.p2_block.rollSetpoint = getRollAngleSetpoint();
            statusData->data.p2_block.pitchRateSetpoint = getPitchRateSetpoint();
            statusData->data.p2_block.pitchSetpoint = getPitchAngleSetpoint();
            statusData->data.p2_block.throttleSetpoint = getThrottleSetpoint();
            statusData->data.p2_block.yawRateSetpoint = getYawRateSetpoint();
            statusData->data.p2_block.headingSetpoint = getHeadingSetpoint();
            statusData->data.p2_block.altitudeSetpoint = getAltitudeSetpoint();
            statusData->data.p2_block.flapSetpoint = getFlapSetpoint();
            statusData->data.p2_block.wirelessConnection = (input_RC_UHFSwitch < -429) << 1;//+ RSSI;
            statusData->data.p2_block.autopilotActive = input_RC_Switch1 > 380;
            statusData->data.p2_block.gpsStatus = gps_Satellites + (gps_PositionFix << 4);
            //statusData->data.p2_block.pathChecksum =
            statusData->data.p2_block.numWaypoints = waypointCount;
            statusData->data.p2_block.waypointIndex = waypointIndex;
            //statusData->data.p2_block.following = <true/false>
            break;
        case PRIORITY3:
            statusData->data.p3_block.rollKD = getGain(ROLL,GAIN_KD);
            statusData->data.p3_block.rollKP = getGain(ROLL,GAIN_KP);
            statusData->data.p3_block.rollKI = getGain(ROLL,GAIN_KI);
            statusData->data.p3_block.pitchKD = getGain(PITCH,GAIN_KD);
            statusData->data.p3_block.pitchKP = getGain(PITCH,GAIN_KP);
            statusData->data.p3_block.pitchKI = getGain(PITCH,GAIN_KI);
            statusData->data.p3_block.yawKD = getGain(YAW,GAIN_KD);
            statusData->data.p3_block.yawKP = getGain(YAW,GAIN_KP);
            statusData->data.p3_block.yawKI = getGain(YAW, GAIN_KI);
            statusData->data.p3_block.headingKD = getGain(HEADING, GAIN_KD);
            statusData->data.p3_block.headingKP = getGain(HEADING, GAIN_KP);
            statusData->data.p3_block.headingKI = getGain(HEADING, GAIN_KI);
            statusData->data.p3_block.altitudeKD = getGain(ALTITUDE, GAIN_KD);
            statusData->data.p3_block.altitudeKP = getGain(ALTITUDE, GAIN_KP);
            statusData->data.p3_block.altitudeKI = getGain(ALTITUDE, GAIN_KI);
            statusData->data.p3_block.throttleKD = getGain(THROTTLE, GAIN_KD);
            statusData->data.p3_block.throttleKP = getGain(THROTTLE, GAIN_KP);
            statusData->data.p3_block.throttleKI = getGain(THROTTLE, GAIN_KI);
            statusData->data.p3_block.flapKD = getGain(FLAP, GAIN_KD);
            statusData->data.p3_block.flapKP = getGain(FLAP, GAIN_KP);
            statusData->data.p3_block.flapKI = getGain(FLAP, GAIN_KI);
            statusData->data.p3_block.cameraStatus = cameraCounter;
            break;
                
        default:
            break;
    }


    if (BLOCKING_MODE) {
        sendTelemetryBlock(statusData);
        destroyTelemetryBlock(statusData);
    } else {
        return pushOutboundTelemetryQueue(statusData);
    }
         
    return 0;

}

void adjustVNOrientationMatrix(float* adjustment){

    adjustment[0] = deg2rad(adjustment[0]);
    adjustment[1] = deg2rad(adjustment[1]);
    adjustment[2] = deg2rad(adjustment[2]);

    float matrix[9];
    VN100_SPI_GetRefFrameRot(0, (float*)&matrix);
    
    float refRotationMatrix[9] = {cos(adjustment[1]) * cos(adjustment[2]), -cos(adjustment[1]) * sin(adjustment[2]), sin(adjustment[1]),
        sin(deg2rad(adjustment[0])) * sin(adjustment[1]) * cos(adjustment[2]) + sin(adjustment[2]) * cos(adjustment[0]), -sin(adjustment[0]) * sin(adjustment[1]) * sin(adjustment[2]) + cos(adjustment[2]) * cos(adjustment[0]), -sin(adjustment[0]) * cos(adjustment[1]),
        -cos(deg2rad(adjustment[0])) * sin(adjustment[1]) * cos(adjustment[2]) + sin(adjustment[2]) * sin(adjustment[0]), cos(adjustment[0]) * sin(adjustment[1]) * sin(adjustment[2]) + cos(adjustment[2]) * sin(adjustment[0]), cos(adjustment[0]) * cos(adjustment[1])};

    int i = 0;
    for (i = 0; i < 9; i++){
        refRotationMatrix[i] += matrix[i];
    }

    VN100_SPI_SetRefFrameRot(0, (float*)&refRotationMatrix);
    VN100_SPI_WriteSettings(0);
    VN100_SPI_Reset(0);

}

void setVNOrientationMatrix(float* angleOffset){
    //angleOffset[0] = x, angleOffset[1] = y, angleOffset[2] = z
    angleOffset[0] = deg2rad(angleOffset[0]);
    angleOffset[1] = deg2rad(angleOffset[1]);
    angleOffset[2] = deg2rad(angleOffset[2]);

    refRotationMatrix[0] = cos(angleOffset[1]) * cos(angleOffset[2]);
    refRotationMatrix[1] = -cos(angleOffset[1]) * sin(angleOffset[2]);
    refRotationMatrix[2] = sin(angleOffset[1]);

    refRotationMatrix[3] = sin(angleOffset[0]) * sin(angleOffset[1]) * cos(angleOffset[2]) + sin(angleOffset[2]) * cos(angleOffset[0]);
    refRotationMatrix[4] = -sin(angleOffset[0]) * sin(angleOffset[1]) * sin(angleOffset[2]) + cos(angleOffset[2]) * cos(angleOffset[0]);
    refRotationMatrix[5] = -sin(angleOffset[0]) * cos(angleOffset[1]);

    refRotationMatrix[6] = -cos(angleOffset[0]) * sin(angleOffset[1]) * cos(angleOffset[2]) + sin(angleOffset[2]) * sin(angleOffset[0]);
    refRotationMatrix[7] = cos(angleOffset[0]) * sin(angleOffset[1]) * sin(angleOffset[2]) + cos(angleOffset[2]) * sin(angleOffset[0]);
    refRotationMatrix[8] = cos(angleOffset[0]) * cos(angleOffset[1]);
    VN100_SPI_SetRefFrameRot(0, (float*)&refRotationMatrix);
    VN100_SPI_WriteSettings(0);
    VN100_SPI_Reset(0);
}
void setAngularWalkVariance(float variance){
    float previousVariance[10];
    VN100_SPI_GetFiltMeasVar(0, (float*)&previousVariance);
    previousVariance[0] = variance;
    VN100_SPI_SetFiltMeasVar(0, (float*)&previousVariance);
    VN100_SPI_WriteSettings(0);
}

void setGyroVariance(float variance){
    float previousVariance[10];
    VN100_SPI_GetFiltMeasVar(0, (float*)&previousVariance);
    previousVariance[1] = variance; //X -Can be split up later if needed
    previousVariance[2] = variance; //Y
    previousVariance[3] = variance; //Z
    VN100_SPI_SetFiltMeasVar(0, (float*)&previousVariance);
    VN100_SPI_WriteSettings(0);
}

void setMagneticVariance(float variance){
    float previousVariance[10];
    VN100_SPI_GetFiltMeasVar(0, (float*)&previousVariance);
    previousVariance[4] = variance; //X -Can be split up later if needed
    previousVariance[5] = variance; //Y
    previousVariance[6] = variance; //Z
    VN100_SPI_SetFiltMeasVar(0, (float*)&previousVariance);
    VN100_SPI_WriteSettings(0);
}

void setAccelVariance(float variance){
    float previousVariance[10];
    VN100_SPI_GetFiltMeasVar(0, (float*)&previousVariance);
    previousVariance[7] = variance; //X -Can be split up later if needed
    previousVariance[8] = variance; //Y
    previousVariance[9] = variance; //Z
    VN100_SPI_SetFiltMeasVar(0, (float*)&previousVariance);
    VN100_SPI_WriteSettings(0);
}

char generateAMDataDMAChecksum(void){
    return 0xAB;
}