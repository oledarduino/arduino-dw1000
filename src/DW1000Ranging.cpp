/*
 * Copyright (c) 2015 by Thomas Trojer <thomas@trojer.net> and Leopold Sayous <leosayous@gmail.com>
 * Decawave DW1000 library for arduino.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @file DW1000Ranging.h
 * Arduino global library (source file) working with the DW1000 library 
 * for the Decawave DW1000 UWB transceiver IC.
 */
 

#include "DW1000Ranging.h"
#include "DW1000Device.h"

DW1000RangingClass DW1000Ranging;

 
//other devices we are going to communicate with which are on our network:
DW1000Device DW1000RangingClass::_networkDevices[MAX_DEVICES];
byte DW1000RangingClass::_currentAddress[8];

//module type (anchor or tag)
int DW1000RangingClass::_type;
// message flow state
volatile byte DW1000RangingClass::_expectedMsgId;
// message sent/received state
volatile boolean DW1000RangingClass::_sentAck=false;
volatile boolean DW1000RangingClass::_receivedAck=false;
// protocol error state
boolean DW1000RangingClass::_protocolFailed=false;
// timestamps to remember


// data buffer
byte DW1000RangingClass::data[LEN_DATA];
// reset line to the chip
unsigned int DW1000RangingClass::_RST;
unsigned int DW1000RangingClass::_SS;
// watchdog and reset period
unsigned long DW1000RangingClass::_lastActivity;
unsigned long DW1000RangingClass::_resetPeriod;
// reply times (same on both sides for symm. ranging)
unsigned int DW1000RangingClass::_replyDelayTimeUS;
// ranging counter (per second)
unsigned int DW1000RangingClass::_successRangingCount=0;
unsigned long DW1000RangingClass::_rangingCountPeriod=0;
//Here our handlers
void (*DW1000RangingClass::_handleNewRange)(void) = 0;




/* ###########################################################################
 * #### Init and end #######################################################
 * ######################################################################### */

void DW1000RangingClass::initCommunication(unsigned int RST, unsigned int SS){
    // reset line to the chip
    _RST = RST;
    _SS = SS;
    _resetPeriod = DEFAULT_RESET_PERIOD;
    // reply times (same on both sides for symm. ranging)
    _replyDelayTimeUS = DEFAULT_REPLY_DELAY_TIME;
    
    DW1000.begin(0, RST);
    DW1000.select(SS);
}
 

void DW1000RangingClass::configureNetwork(unsigned int deviceAddress, unsigned int networkId, const byte mode[]){
    // general configuration
    DW1000.newConfiguration();
    DW1000.setDefaults();
    DW1000.setDeviceAddress(deviceAddress);
    DW1000.setNetworkId(networkId);
    DW1000.enableMode(mode);
    DW1000.commitConfiguration();
    
}

void DW1000RangingClass::generalStart(){
    // attach callback for (successfully) sent and received messages
    DW1000.attachSentHandler(handleSent);
    DW1000.attachReceivedHandler(handleReceived);
    // anchor starts in receiving mode, awaiting a ranging poll message
    
    
    if(DEBUG){
        // DEBUG monitoring
        Serial.println("DW1000-arduino");
        // initialize the driver
        
        
        Serial.println("configuration..");
        // DEBUG chip info and registers pretty printed
        char msg[90];
        DW1000.getPrintableDeviceIdentifier(msg);
        Serial.print("Device ID: "); Serial.println(msg);
        DW1000.getPrintableExtendedUniqueIdentifier(msg);
        Serial.print("Unique ID: "); Serial.println(msg);
        DW1000.getPrintableNetworkIdAndShortAddress(msg);
        Serial.print("Network ID & Device Address: "); Serial.println(msg);
        DW1000.getPrintableDeviceMode(msg);
        Serial.print("Device mode: "); Serial.println(msg);
    }
    
    
    // anchor starts in receiving mode, awaiting a ranging poll message
    receiver();
    noteActivity();
    // for first time ranging frequency computation
    _rangingCountPeriod = millis();
}


void DW1000RangingClass::startAsAnchor(DW1000Device myDevice, DW1000Device networkDevices[]){
    //we copy our network array into _networkDevices 
    memcpy(_networkDevices, networkDevices, sizeof(*networkDevices));
    //we set the address of our device:
    byte address[8];
    myDevice.getAddress(address);
    
    //we copy the address in an array
    memcpy(_currentAddress, address, 8);
    
    DW1000.setEUI(address);
    
    //general start:
    generalStart();
    
    //defined type as anchor
    _type=ANCHOR;
    
    if(DEBUG){
        Serial.println("### ANCHOR ###");
    }
}

void DW1000RangingClass::startAsTag(DW1000Device myDevice, DW1000Device networkDevices[]){
    //we copy our network array into _networkDevices
    memcpy(_networkDevices, networkDevices, sizeof(DW1000Device));
    
    byte address[8];
    myDevice.getAddress(address);
    
    DW1000.setEUI(address);
    
    generalStart();
    //defined type as anchor
    _type=TAG;
    
    if(DEBUG){
        Serial.println("### TAG ###");
    }
    //we can start to poll: (this is the TAG which start to poll)
    transmitPoll();
}

/* ###########################################################################
 * #### Setters and Getters ##################################################
 * ######################################################################### */

//setters
void DW1000RangingClass::setReplyTime(unsigned int replyDelayTimeUs){ _replyDelayTimeUS=replyDelayTimeUs;}
void DW1000RangingClass::setResetPeriod(unsigned long resetPeriod){ _resetPeriod=resetPeriod;}


//getters
void DW1000RangingClass::getCurrentAddress(byte Address[]){
    memcpy(Address, _currentAddress, 8);
}
void DW1000RangingClass::getCurrentShortAddress(byte Address[]){
    memcpy(Address, _currentShortAddress, 2);
}


DW1000Device* DW1000RangingClass::getDistantDevice(){
    //we get the device which correspond to the message which was sent (need to be filtered by MAC address)
    return &_networkDevices[0];
}




/* ###########################################################################
 * #### Public methods #######################################################
 * ######################################################################### */

void DW1000RangingClass::checkForReset(){
    long curMillis = millis();
    if(!_sentAck && !_receivedAck) {
        // check if inactive
        if(curMillis - _lastActivity > _resetPeriod) {
            resetInactive();
        }
        return;
    }
}

byte DW1000RangingClass::detectMessageType(byte data[]){
    if(data[0]==0xC5)
    {
        return BLINK;
    }
    else if(data[0]==FC_1 && data[1]==FC_2 && data[LONG_MAC_LEN]==RANGING_INIT)
    {
        return RANGING_INIT;
    }
}

void DW1000RangingClass::loop(){
    //we check if needed to reset !
    checkForReset();
        
    
    if(_sentAck){
        _sentAck = false;
        
        //we get the device which correspond to the message which was sent (need to be filtered by MAC address)
        DW1000Device *myDistantDevice=&_networkDevices[0];
        
        //A msg was sent. We launch the ranging protocole when a message was sent
        if(_type==ANCHOR){
            if(data[0] == POLL_ACK) {
                DW1000.getTransmitTimestamp(myDistantDevice->timePollAckSent);
                noteActivity();
            }
        }
        else if(_type==TAG){
            if(data[0] == POLL) {
                DW1000.getTransmitTimestamp(myDistantDevice->timePollSent);
                //Serial.print("Sent POLL @ "); Serial.println(timePollSent.getAsFloat());
            } else if(data[0] == RANGE) {
                DW1000.getTransmitTimestamp(myDistantDevice->timeRangeSent);
                noteActivity();
            }
        }
        
    }
    
    //check for new received message
    if(_receivedAck){
        _receivedAck=false;
         
        
        //we get the device which correspond to the message which was sent (need to be filtered by MAC address)
        DW1000Device *myDistantDevice=&_networkDevices[0];
        
        //we read the datas from the modules:
        // get message and parse
        DW1000.getData(data, LEN_DATA);
        
        //then we proceed to range protocole
        if(_type==ANCHOR){
            if(data[0] != _expectedMsgId) {
                // unexpected message, start over again (except if already POLL)
                _protocolFailed = true;
            }
            if(data[0] == POLL) {
                // on POLL we (re-)start, so no protocol failure
                _protocolFailed = false;
                DW1000.getReceiveTimestamp(myDistantDevice->timePollReceived);
                _expectedMsgId = RANGE;
                transmitPollAck();
                noteActivity();
            }
            else if(data[0] == RANGE) {
                DW1000.getReceiveTimestamp(myDistantDevice->timeRangeReceived);
                _expectedMsgId = POLL;
                if(!_protocolFailed) {
                    myDistantDevice->timePollSent.setTimestamp(data+1);
                    myDistantDevice->timePollAckReceived.setTimestamp(data+6);
                    myDistantDevice->timeRangeSent.setTimestamp(data+11);
                    
                    // (re-)compute range as two-way ranging is done
                    DW1000Time myTOF;
                    computeRangeAsymmetric(myDistantDevice, &myTOF); // CHOSEN RANGING ALGORITHM
                    
                    float distance=myTOF.getAsMeters();
                    
                    myDistantDevice->setRXPower(DW1000.getReceivePower());
                    float rangeBias=rangeRXCorrection(myDistantDevice->getRXPower());
                    myDistantDevice->setRange(distance-rangeBias);
                     
                    myDistantDevice->setFPPower(DW1000.getFirstPathPower());
                    myDistantDevice->setQuality(DW1000.getReceiveQuality());
                    
                    //we wend the range to TAG
                    transmitRangeReport(myDistantDevice);
                    
                    
                    
                    
                    //we have finished our range computation. We send the corresponding handler
                    
                    if(_handleNewRange != 0) {
                        (*_handleNewRange)();
                    }
                    
                }
                else {
                    transmitRangeFailed();
                }
                
                noteActivity();
            }
        }
        else if(_type==TAG){
            
            // get message and parse
            if(data[0] != _expectedMsgId) {
                // unexpected message, start over again
                //Serial.print("Received wrong message # "); Serial.println(msgId);
                _expectedMsgId = POLL_ACK;
                transmitPoll();
                return;
            }
            if(data[0] == POLL_ACK) {
                DW1000.getReceiveTimestamp(myDistantDevice->timePollAckReceived);
                _expectedMsgId = RANGE_REPORT;
                transmitRange(myDistantDevice);
                noteActivity();
            }
            else if(data[0] == RANGE_REPORT) {
                _expectedMsgId = POLL_ACK;
                float curRange;
                memcpy(&curRange, data+1, 4);
                float curRXPower;
                memcpy(&curRXPower, data+5, 4);
                //we have a new range to save !
                myDistantDevice->setRange(curRange);
                myDistantDevice->setRXPower(curRXPower);
                
                //We can call our handler !
                if(_handleNewRange != 0){
                    (*_handleNewRange)();
                }
                
                //we start again ranging
                transmitPoll();
                noteActivity();
            }
            else if(data[0] == RANGE_FAILED) {
                _expectedMsgId = POLL_ACK;
                transmitPoll();
                noteActivity();
            }
        }

    }
}










/* ###########################################################################
 * #### Private methods and Handlers for transmit & Receive reply ############
 * ######################################################################### */


void DW1000RangingClass::handleSent() {
    
    // status change on sent success
    _sentAck = true;
    
    
    
}

void DW1000RangingClass::handleReceived() {
    
    
    // status change on received success
    _receivedAck = true;
    
  
}


void DW1000RangingClass::noteActivity() {
    // update activity timestamp, so that we do not reach "resetPeriod"
    _lastActivity = millis();
}

void DW1000RangingClass::resetInactive() {
    //if inactive
    Serial.println("---- RESET INACTIVE ---");
    if(_type==ANCHOR){
        _expectedMsgId = POLL;
        receiver();
    }
    else if(_type==TAG){
        _expectedMsgId = POLL_ACK;
        transmitPoll();
    }
    noteActivity();
}



/* ###########################################################################
 * #### Methods for ranging protocole   ######################################
 * ######################################################################### */



void DW1000RangingClass::transmit(byte data[]){
    DW1000.newTransmit();
    DW1000.setDefaults();
    DW1000.setData(data, LEN_DATA);
    DW1000.startTransmit();
}


void DW1000RangingClass::transmit(byte data[], DW1000Time time){
    DW1000.newTransmit();
    DW1000.setDefaults();
    DW1000.setDelay(time);
    DW1000.setData(data, LEN_DATA);
    DW1000.startTransmit();
}

void DW1000RangingClass::transmitBlink(){
    DW1000Mac mac;
    mac.generateBlinkFrame(data);
    transmit(data);
}

void DW1000RangingClass::transmitPoll() {
    data[0] = POLL;
    transmit(data);
}


void DW1000RangingClass::transmitPollAck() {
    data[0] = POLL_ACK;
    // delay the same amount as ranging tag
    DW1000Time deltaTime = DW1000Time(_replyDelayTimeUS, DW_MICROSECONDS);
    transmit(data, deltaTime);
}

void DW1000RangingClass::transmitRange(DW1000Device *myDistantDevice) {
    data[0] = RANGE;
    // delay sending the message and remember expected future sent timestamp
    DW1000Time deltaTime = DW1000Time(_replyDelayTimeUS, DW_MICROSECONDS);
    //we get the device which correspond to the message which was sent (need to be filtered by MAC address)
    myDistantDevice->timeRangeSent = DW1000.setDelay(deltaTime);
    myDistantDevice->timePollSent.getTimestamp(data+1);
    myDistantDevice->timePollAckReceived.getTimestamp(data+6);
    myDistantDevice->timeRangeSent.getTimestamp(data+11);
    transmit(data);
    //Serial.print("Expect RANGE to be sent @ "); Serial.println(timeRangeSent.getAsFloat());
}


void DW1000RangingClass::transmitRangeReport(DW1000Device *myDistantDevice) {
    data[0] = RANGE_REPORT;
    // write final ranging result
    float curRange=myDistantDevice->getRange();
    float curRXPower=myDistantDevice->getRXPower();
    //We add the Range and then the RXPower
    memcpy(data+1, &curRange, 4);
    memcpy(data+5, &curRXPower, 4);
    transmit(data);
}

void DW1000RangingClass::transmitRangeFailed() {
    data[0] = RANGE_FAILED;
    transmit(data);
}

void DW1000RangingClass::receiver() {
    DW1000.newReceive();
    DW1000.setDefaults();
    // so we don't need to restart the receiver manually
    DW1000.receivePermanently(true);
    DW1000.startReceive();
}









/* ###########################################################################
 * #### Methods for range computation and corrections  #######################
 * ######################################################################### */


void DW1000RangingClass::computeRangeAsymmetric(DW1000Device *myDistantDevice, DW1000Time *myTOF) {
    // asymmetric two-way ranging (more computation intense, less error prone)
    DW1000Time round1 = (myDistantDevice->timePollAckReceived-myDistantDevice->timePollSent).wrap();
    DW1000Time reply1 = (myDistantDevice->timePollAckSent-myDistantDevice->timePollReceived).wrap();
    DW1000Time round2 = (myDistantDevice->timeRangeReceived-myDistantDevice->timePollAckSent).wrap();
    DW1000Time reply2 = (myDistantDevice->timeRangeSent-myDistantDevice->timePollAckReceived).wrap();
    myTOF->setTimestamp((round1 * round2 - reply1 * reply2) / (round1 + round2 + reply1 + reply2));
}




// ----  Range RX correction ----



// {RSL, PRF 16MHz, PRF 64 MHz}
//all is pass in int to optimize SRAM memory !
//In order to keep the dot, we multiply all by 10 and we will dividide
//by 10 after !
//17 bytes in SRAM
char DW1000RangingClass::_bias_RSL[17]={-61,-63,-65,-67,-69,-71,-73,-75,-77,-79,-81,-83,-85,-87,-89,-91,-93};
//17*2=34 bytes in SRAM
short DW1000RangingClass::_bias_PRF_16[17]={-198,-187,-179,-163,-143,-127,-109,-84,-59,-31,0,36,65,84,97,106,110};
//17 bytes in SRAM
char DW1000RangingClass::_bias_PRF_64[17]={-110,-105,-100,-93,-82,-69,-51,-27,0,21,35,42,49,62,71,76,81};
// => total of 68 bytes !



float DW1000RangingClass::rangeRXCorrection(float RXPower){
    byte PRF=DW1000.getPulseFrequency();
    float rangeBias=0;
    if(PRF==DW1000.TX_PULSE_FREQ_16MHZ)
    {
        rangeBias=computeRangeBias_16(RXPower);
    }
    else if(PRF==DW1000.TX_PULSE_FREQ_64MHZ)
    {
        rangeBias=computeRangeBias_64(RXPower);
    } 
    
}


float DW1000RangingClass::computeRangeBias_16(float RXPower)
{
    //We test first boundary
    if(RXPower>=_bias_RSL[0])
        return float(_bias_PRF_16[0])/1000.0f;
    
    //we test last boundary
    if(RXPower<=_bias_RSL[16])
        return float(_bias_PRF_16[16])/1000.0f;

    
    for(int i=1; i<17; i++){
        //we search for the position we are. All is in negative !
        if(RXPower<_bias_RSL[i-1] && RXPower>_bias_RSL[i]){
            //we have our position i. We now need to calculate the line
            float a=float(_bias_PRF_16[i-1]/10.0f-_bias_PRF_16[i]/10.0f)/float(_bias_RSL[i-1]-_bias_RSL[i]);
            float b=_bias_PRF_16[i-1]/10.0f - a * _bias_RSL[i-1];
            //return our bias
            return (a*RXPower + b)/100.0f;
        }
    }
}

float DW1000RangingClass::computeRangeBias_64(float RXPower)
{
    //We test first boundary
    if(RXPower>=_bias_RSL[0])
    {
        Serial.println("return first boundary");
        return float(_bias_PRF_64[0]/1000.0f);
    }
    
    //we test last boundary
    if(RXPower<=_bias_RSL[16])
    {
        Serial.println("Return last boundary");
        return float(_bias_PRF_64[16]/1000.0f);
    }
    
    
    for(int i=1; i<17; i++){
        //we search for the position we are. All is in negative !
        if(RXPower<_bias_RSL[i-1] && RXPower>_bias_RSL[i]){
            //we have our position i. We now need to calculate the line
            float a=float(_bias_PRF_64[i-1]/10.0f-_bias_PRF_64[i]/10.0f)/float(_bias_RSL[i-1]-_bias_RSL[i]);
            float b=_bias_PRF_64[i-1]/10.0f - a * _bias_RSL[i-1];
            //return our bias
            return (a*RXPower + b)/100.0f;
        }
    }
}




