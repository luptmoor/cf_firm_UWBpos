#include "system.h"
#include "FreeRTOS.h"
#include "task.h"
#include "commander.h"
#include "relative_localization.h"
#include "num.h"
#include "param.h"
#include "debug.h"
#include <stdlib.h> // random
#include "lpsTwrTag.h" // UWBNum
#include "configblock.h"
#include "uart2.h"
#include "log.h"
#define USE_MONOCAM 0

static bool isInit;
static bool onGround = true;
static uint8_t keepFlying = 0;
static setpoint_t setpoint;
static float_t relaVarInCtrl[NumUWB][STATE_DIM_rl];
static uint8_t selfID;
static float_t height;

static float relaCtrl_p = 2.0f;
static float relaCtrl_i = 0.0001f;
static float relaCtrl_d = 0.01f;
static char c = 0; // monoCam

static void setHoverSetpoint(setpoint_t *setpoint, float vx, float vy, float z, float yawrate)
{
  setpoint->mode.z = modeAbs;
  setpoint->position.z = z;
  setpoint->mode.yaw = modeVelocity;
  setpoint->attitudeRate.yaw = yawrate;
  setpoint->mode.x = modeVelocity;
  setpoint->mode.y = modeVelocity;
  setpoint->velocity.x = vx;
  setpoint->velocity.y = vy;
  setpoint->velocity_body = true;
  commanderSetSetpoint(setpoint, 3);
}

static void flyRandomIn1meter(void){
  float_t randomYaw = (rand() / (float)RAND_MAX) * 6.28f; // 0-2pi rad
  float_t randomVel = (rand() / (float)RAND_MAX); // 0-1 m/s
  float_t vxBody = randomVel * cosf(randomYaw);
  float_t vyBody = randomVel * sinf(randomYaw);
  for (int i=1; i<100; i++) {
    setHoverSetpoint(&setpoint, vxBody, vyBody, height, 0);
    vTaskDelay(M2T(10));
  }
  for (int i=1; i<100; i++) {
    setHoverSetpoint(&setpoint, -vxBody, -vyBody, height, 0);
    vTaskDelay(M2T(10));
  }
}

#if USE_MONOCAM
static float PreErr_Ypos = 0;
static float IntErr_Ypos = 0;
static uint32_t PreTimeYpos;
static uint32_t PreTimeYaw = 0;
static bool wait = false; ////
static uint32_t PreTimeFlyX;
static void flyViaDoorNoYaw(char camYpos){
  static float_t tarYaw = 0.0f;
  static float_t vx;
  static float_t vy;
  if(camYpos){
    float dt = (float)(xTaskGetTickCount()-PreTimeYpos)/configTICK_RATE_HZ;
    PreTimeYpos = xTaskGetTickCount();
    if(dt > 1) // skip the first run
      return;
    // pid control for door flight
    float err_Ypos;
    err_Ypos = -(camYpos - 64); // 0-128, nominal value = 64
    float pid_vYpos = 1.0f * err_Ypos;
    float dYpos = (err_Ypos - PreErr_Ypos) / dt;
    PreErr_Ypos = err_Ypos;
    pid_vYpos += 0.01f * dYpos;
    IntErr_Ypos += err_Ypos * dt;
    pid_vYpos += 0.0001f * constrain(IntErr_Ypos, -10.0f, 10.0f);
    pid_vYpos = pid_vYpos/64;
    pid_vYpos = constrain(pid_vYpos, -0.6f, 0.6f);  
    setHoverSetpoint(&setpoint, 0.4f, pid_vYpos, 0.5f, 0);
    tarYaw = 0.0f;
    PreTimeFlyX = xTaskGetTickCount();
    wait = true;
  }else{
    if(wait){
      setHoverSetpoint(&setpoint, 0.5, 0, 0.5f, 0);
      if(xTaskGetTickCount()-PreTimeFlyX > 1000){
        wait = false;
      }
    }else{
      if(xTaskGetTickCount() - PreTimeYaw > 100){
        tarYaw += 0.04f;
        PreTimeYaw = xTaskGetTickCount();
        vx = 1.0f*cosf(tarYaw);
        vy = 1.0f*sinf(tarYaw);
      }
      setHoverSetpoint(&setpoint, vx, vy, 0.5f, 0);
    }
  }
}
#endif

static float_t targetX;
static float_t targetY;
static float PreErr_x = 0;
static float PreErr_y = 0;
static float IntErr_x = 0;
static float IntErr_y = 0;
static uint32_t PreTime;
static void formation0asCenter(float_t tarX, float_t tarY){
  float dt = (float)(xTaskGetTickCount()-PreTime)/configTICK_RATE_HZ;
  PreTime = xTaskGetTickCount();
  if(dt > 1) // skip the first run of the EKF
    return;
  // pid control for formation flight
  float err_x = -(tarX - relaVarInCtrl[0][STATE_rlX]);
  float err_y = -(tarY - relaVarInCtrl[0][STATE_rlY]);
  float pid_vx = relaCtrl_p * err_x;
  float pid_vy = relaCtrl_p * err_y;
  float dx = (err_x - PreErr_x) / dt;
  float dy = (err_y - PreErr_y) / dt;
  PreErr_x = err_x;
  PreErr_y = err_y;
  pid_vx += relaCtrl_d * dx;
  pid_vy += relaCtrl_d * dy;
  IntErr_x += err_x * dt;
  IntErr_y += err_y * dt;
  pid_vx += relaCtrl_i * constrain(IntErr_x, -0.5, 0.5);
  pid_vy += relaCtrl_i * constrain(IntErr_y, -0.5, 0.5);
  pid_vx = constrain(pid_vx, -1.5f, 1.5f);
  pid_vy = constrain(pid_vy, -1.5f, 1.5f);  
  setHoverSetpoint(&setpoint, pid_vx, pid_vy, height, 0);
}

void relativeControlTask(void* arg)
{
  static const float_t targetList[7][STATE_DIM_rl]={{0.0f, 0.0f, 0.0f}, {-1.0f, -0.2f, 0.0f}, {-1.3f, -0.2f, 0.0f}, {-1.0f, -1.5f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {-2.0f, 0.0f, 0.0f}};
  static uint32_t ctrlTick;
  systemWaitStart();
  // height = (float)selfID*0.1f+0.2f;
  height = 0.5f;
  while(1) {
    vTaskDelay(10);
#if USE_MONOCAM
    if(selfID==0)
      uart2Getchar(&c);
#endif
    if(relativeInfoRead((float_t *)relaVarInCtrl) && keepFlying){
      // take off
      if(onGround){
        for (int i=0; i<5; i++) {
          setHoverSetpoint(&setpoint, 0, 0, 0.3f, 0);
          vTaskDelay(M2T(100));
        }
        onGround = false;
        ctrlTick = xTaskGetTickCount();
      }

      // control loop
      // setHoverSetpoint(&setpoint, 0, 0, height, 0); // hover
      uint32_t tickInterval = xTaskGetTickCount() - ctrlTick;
      if( tickInterval < 20000){
        flyRandomIn1meter(); // random flight within first 10 seconds
        targetX = relaVarInCtrl[0][STATE_rlX];
        targetY = relaVarInCtrl[0][STATE_rlY];
      }
      else
      {

#if USE_MONOCAM
        if ( (tickInterval > 20000) && (tickInterval < 35000) ){ // 0-random, other formation
          flyRandomIn1meter();
        }

        if ( (tickInterval > 35000) && (tickInterval < 50000) ){
          flyRandomIn1meter();
        }

        if (tickInterval > 50000){
          flyViaDoorNoYaw(c);
        }
#else
        if ( (tickInterval > 20000) && (tickInterval < 30000) ){ // 0-random, other formation
          formation0asCenter(targetX, targetY); 
        }

        if ( (tickInterval > 30000) && (tickInterval < 50000) ){
          targetX = -cosf(relaVarInCtrl[0][STATE_rlYaw])*targetList[selfID][STATE_rlX] + sinf(relaVarInCtrl[0][STATE_rlYaw])*targetList[selfID][STATE_rlY];
          targetY = -sinf(relaVarInCtrl[0][STATE_rlYaw])*targetList[selfID][STATE_rlX] - cosf(relaVarInCtrl[0][STATE_rlYaw])*targetList[selfID][STATE_rlY];
          formation0asCenter(targetX, targetY); 
        }

        if (tickInterval > 50000){
          float_t vx0;
          float_t vy0;
          relativeVel0Read(&vx0, &vy0, 0);
          float_t tarXin0body = -vx0/sqrtf(vx0*vx0+vy0*vy0);
          float_t tarYin0body = -vy0/sqrtf(vx0*vx0+vy0*vy0);
          targetX = -cosf(relaVarInCtrl[0][STATE_rlYaw])*tarXin0body + sinf(relaVarInCtrl[0][STATE_rlYaw])*tarYin0body;
          targetY = -sinf(relaVarInCtrl[0][STATE_rlYaw])*tarXin0body - cosf(relaVarInCtrl[0][STATE_rlYaw])*tarYin0body;
          formation0asCenter(targetX, targetY);
        }
#endif
      }

    }else{
      // landing procedure
      if(!onGround){
        for (int i=1; i<5; i++) {
          setHoverSetpoint(&setpoint, 0, 0, 0.3f-(float)i*0.05f, 0);
          vTaskDelay(M2T(10));
        }
        onGround = true;
      } 
    }
  }
}

void relativeControlInit(void)
{
  if (isInit)
    return;
  selfID = (uint8_t)(((configblockGetRadioAddress()) & 0x000000000f) - 5);
#if USE_MONOCAM
  if(selfID==0)
    uart2Init(115200); // only CF0 has monoCam and usart comm
#endif
  xTaskCreate(relativeControlTask,"relative_Control",configMINIMAL_STACK_SIZE, NULL,3,NULL );
  isInit = true;
}

PARAM_GROUP_START(relative_ctrl)
PARAM_ADD(PARAM_UINT8, keepFlying, &keepFlying)
PARAM_ADD(PARAM_FLOAT, relaCtrl_p, &relaCtrl_p)
PARAM_ADD(PARAM_FLOAT, relaCtrl_i, &relaCtrl_i)
PARAM_ADD(PARAM_FLOAT, relaCtrl_d, &relaCtrl_d)
PARAM_GROUP_STOP(relative_ctrl)

LOG_GROUP_START(mono_cam)
LOG_ADD(LOG_UINT8, charCam, &c)
LOG_GROUP_STOP(mono_cam)