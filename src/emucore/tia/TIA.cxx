//============================================================================
//
//   SSSS    tt          lll  lll
//  SS  SS   tt           ll   ll
//  SS     tttttt  eeee   ll   ll   aaaa
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     ttt  eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2018 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//============================================================================

#include "TIA.hxx"
#include "M6502.hxx"
#include "Console.hxx"
#include "Control.hxx"
#include "Paddles.hxx"
#include "DelayQueueIteratorImpl.hxx"
#include "TIAConstants.hxx"
#include "frame-manager/FrameManager.hxx"
#include "AudioQueue.hxx"
#include "DispatchResult.hxx"

#ifdef DEBUGGER_SUPPORT
  #include "CartDebug.hxx"
#endif

enum CollisionMask: uInt32 {
  player0   = 0b0111110000000000,
  player1   = 0b0100001111000000,
  missile0  = 0b0010001000111000,
  missile1  = 0b0001000100100110,
  ball      = 0b0000100010010101,
  playfield = 0b0000010001001011
};

enum Delay: uInt8 {
  hmove = 6,
  pf = 2,
  grp = 1,
  shufflePlayer = 1,
  shuffleBall = 1,
  hmp = 2,
  hmm = 2,
  hmbl = 2,
  hmclr = 2,
  refp = 1,
  enabl = 1,
  enam = 1,
  vblank = 1
};

enum ResxCounter: uInt8 {
  hblank = 159,
  lateHblank = 158,
  frame = 157
};

// This parameter still has room for tuning. If we go lower than 73, long005 will show
// a slight artifact (still have to crosscheck on real hardware), if we go lower than
// 70, the G.I. Joe will show an artifact (hole in roof).
static constexpr uInt8 resxLateHblankThreshold = 73;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TIA::TIA(Console& console, Settings& settings)
  : myConsole(console),
    mySettings(settings),
    myFrameManager(nullptr),
    myPlayfield(~CollisionMask::playfield & 0x7FFF),
    myMissile0(~CollisionMask::missile0 & 0x7FFF),
    myMissile1(~CollisionMask::missile1 & 0x7FFF),
    myPlayer0(~CollisionMask::player0 & 0x7FFF),
    myPlayer1(~CollisionMask::player1 & 0x7FFF),
    myBall(~CollisionMask::ball & 0x7FFF),
    mySpriteEnabledBits(0xFF),
    myCollisionsEnabledBits(0xFF)
{
  bool devSettings = mySettings.getBool("dev.settings");
  myTIAPinsDriven = mySettings.getBool(devSettings ? "dev.tiadriven" : "plr.tiadriven");

  myBackground.setTIA(this);
  myPlayfield.setTIA(this);
  myPlayer0.setTIA(this);
  myPlayer1.setTIA(this);
  myMissile0.setTIA(this);
  myMissile1.setTIA(this);
  myBall.setTIA(this);

  myEnableJitter = mySettings.getBool(devSettings ? "dev.tv.jitter" : "plr.tv.jitter");
  myJitterFactor = mySettings.getInt(devSettings ? "dev.tv.jitter_recovery" : "plr.tv.jitter_recovery");

  reset();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::setFrameManager(AbstractFrameManager *frameManager)
{
  clearFrameManager();

  myFrameManager = frameManager;

  myFrameManager->setHandlers(
    [this] () {
      onFrameStart();
    },
    [this] () {
      onFrameComplete();
    }
  );

  myFrameManager->enableJitter(myEnableJitter);
  myFrameManager->setJitterFactor(myJitterFactor);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::setAudioQueue(shared_ptr<AudioQueue> queue)
{
  myAudio.setAudioQueue(queue);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::clearFrameManager()
{
  if (!myFrameManager) return;

  myFrameManager->clearHandlers();

  myFrameManager = nullptr;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::reset()
{
  myHctr = 0;
  myMovementInProgress = false;
  myExtendedHblank = false;
  myMovementClock = 0;
  myPriority = Priority::normal;
  myHstate = HState::blank;
  myCollisionMask = 0;
  myLinesSinceChange = 0;
  myCollisionUpdateRequired = false;
  myColorLossEnabled = myColorLossActive = false;
  myColorHBlank = 0;
  myLastCycle = 0;
  mySubClock = 0;
  myHctrDelta = 0;
  myXAtRenderingStart = 0;

  memset(myShadowRegisters, 0, 64);

  myBackground.reset();
  myPlayfield.reset();
  myMissile0.reset();
  myMissile1.reset();
  myPlayer0.reset();
  myPlayer1.reset();
  myBall.reset();

  myInput0.reset();
  myInput1.reset();

  myAudio.reset();

  myTimestamp = 0;
  for (PaddleReader& paddleReader : myPaddleReaders)
    paddleReader.reset(myTimestamp);

  myDelayQueue.reset();

  myCyclesAtFrameStart = 0;

  if (myFrameManager)
  {
    myFrameManager->reset();
    frameReset();  // Recalculate the size of the display
  }

  myFrontBufferFrameRate = myFrameBufferFrameRate = 0;
  myFrontBufferScanlines = myFrameBufferScanlines = 0;

  myNewFramePending = false;

  // Must be done last, after all other items have reset
  enableFixedColors(mySettings.getBool(mySettings.getBool("dev.settings") ? "dev.debugcolors" : "plr.debugcolors"));
  setFixedColorPalette(mySettings.getString("tia.dbgcolors"));

#ifdef DEBUGGER_SUPPORT
  createAccessBase();
#endif // DEBUGGER_SUPPORT
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::frameReset()
{
  memset(myBackBuffer, 0, 160 * TIAConstants::frameBufferHeight);
  memset(myFrontBuffer, 0, 160 * TIAConstants::frameBufferHeight);
  memset(myFramebuffer, 0, 160 * TIAConstants::frameBufferHeight);
  enableColorLoss(mySettings.getBool("dev.settings") ? "dev.colorloss" : "plr.colorloss");
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::install(System& system)
{
  installDelegate(system, *this);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::installDelegate(System& system, Device& device)
{
  // Remember which system I'm installed in
  mySystem = &system;

  // All accesses are to the given device
  System::PageAccess access(&device, System::PA_READWRITE);

  // Map all peek/poke to mirrors of TIA address space to this class
  // That is, all mirrors of ($00 - $3F) in the lower 4K of the 2600
  // address space are mapped here
  for(uInt16 addr = 0; addr < 0x1000; addr += System::PAGE_SIZE)
    if((addr & TIA_BIT) == 0x0000)
      mySystem->setPageAccess(addr, access);

  mySystem->m6502().setOnHaltCallback(
    [this] () {
      onHalt();
    }
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::save(Serializer& out) const
{
  try
  {
    out.putString(name());

    if(!myDelayQueue.save(out))   return false;
    if(!myFrameManager->save(out)) return false;

    if(!myBackground.save(out)) return false;
    if(!myPlayfield.save(out))  return false;
    if(!myMissile0.save(out))   return false;
    if(!myMissile1.save(out))   return false;
    if(!myPlayer0.save(out))    return false;
    if(!myPlayer1.save(out))    return false;
    if(!myBall.save(out))       return false;
    if(!myAudio.save(out))      return false;

    for (const PaddleReader& paddleReader : myPaddleReaders)
      if(!paddleReader.save(out)) return false;

    if(!myInput0.save(out)) return false;
    if(!myInput1.save(out)) return false;

    out.putBool(myTIAPinsDriven);

    out.putInt(int(myHstate));

    out.putInt(myHctr);
    out.putInt(myHctrDelta);
    out.putInt(myXAtRenderingStart);

    out.putBool(myCollisionUpdateRequired);
    out.putInt(myCollisionMask);

    out.putInt(myMovementClock);
    out.putBool(myMovementInProgress);
    out.putBool(myExtendedHblank);

    out.putInt(myLinesSinceChange);

    out.putInt(int(myPriority));

    out.putByte(mySubClock);
    out.putLong(myLastCycle);

    out.putByte(mySpriteEnabledBits);
    out.putByte(myCollisionsEnabledBits);

    out.putByte(myColorHBlank);

    out.putLong(myTimestamp);

    out.putByteArray(myShadowRegisters, 64);

    out.putLong(myCyclesAtFrameStart);

    out.putInt(myFrameBufferScanlines);
    out.putInt(myFrontBufferScanlines);
    out.putDouble(myFrameBufferFrameRate);
    out.putDouble(myFrontBufferFrameRate);
  }
  catch(...)
  {
    cerr << "ERROR: TIA::save" << endl;
    return false;
  }

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::load(Serializer& in)
{
  try
  {
    if(in.getString() != name())
      return false;

    if(!myDelayQueue.load(in))   return false;
    if(!myFrameManager->load(in)) return false;

    if(!myBackground.load(in)) return false;
    if(!myPlayfield.load(in))  return false;
    if(!myMissile0.load(in))   return false;
    if(!myMissile1.load(in))   return false;
    if(!myPlayer0.load(in))    return false;
    if(!myPlayer1.load(in))    return false;
    if(!myBall.load(in))       return false;
    if(!myAudio.load(in))       return false;

    for (PaddleReader& paddleReader : myPaddleReaders)
      if(!paddleReader.load(in)) return false;

    if(!myInput0.load(in)) return false;
    if(!myInput1.load(in)) return false;

    myTIAPinsDriven = in.getBool();

    myHstate = HState(in.getInt());

    myHctr = in.getInt();
    myHctrDelta = in.getInt();
    myXAtRenderingStart = in.getInt();

    myCollisionUpdateRequired = in.getBool();
    myCollisionMask = in.getInt();

    myMovementClock = in.getInt();
    myMovementInProgress = in.getBool();
    myExtendedHblank = in.getBool();

    myLinesSinceChange = in.getInt();

    myPriority = Priority(in.getInt());

    mySubClock = in.getByte();
    myLastCycle = in.getLong();

    mySpriteEnabledBits = in.getByte();
    myCollisionsEnabledBits = in.getByte();

    myColorHBlank = in.getByte();

    myTimestamp = in.getLong();

    in.getByteArray(myShadowRegisters, 64);

    myCyclesAtFrameStart = in.getLong();

    myFrameBufferScanlines = in.getInt();
    myFrontBufferScanlines = in.getInt();
    myFrameBufferFrameRate = in.getDouble();
    myFrontBufferFrameRate = in.getDouble();
  }
  catch(...)
  {
    cerr << "ERROR: TIA::load" << endl;
    return false;
  }

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::bindToControllers()
{
  myConsole.leftController().setOnAnalogPinUpdateCallback(
    [this] (Controller::AnalogPin pin) {
      updateEmulation();

      switch (pin) {
        case Controller::AnalogPin::Five:
          updatePaddle(1);
          break;

        case Controller::AnalogPin::Nine:
          updatePaddle(0);
          break;
      }
    }
  );

  myConsole.rightController().setOnAnalogPinUpdateCallback(
    [this] (Controller::AnalogPin pin) {
      updateEmulation();

      switch (pin) {
        case Controller::AnalogPin::Five:
          updatePaddle(3);
          break;

        case Controller::AnalogPin::Nine:
          updatePaddle(2);
          break;
      }
    }
  );

  for (uInt8 i = 0; i < 4; i++)
    updatePaddle(i);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::peek(uInt16 address)
{
  updateEmulation();

  // If pins are undriven, we start with the last databus value
  // Otherwise, there is some randomness injected into the mix
  // In either case, we start out with D7 and D6 disabled (the only
  // valid bits in a TIA read), and selectively enable them
  uInt8 lastDataBusValue =
    !myTIAPinsDriven ? mySystem->getDataBusState() : mySystem->getDataBusState(0xFF);

  uInt8 result;

  switch (address & 0x0F) {
    case CXM0P:
      result = collCXM0P();
      break;

    case CXM1P:
      result = collCXM1P();
      break;

    case CXP0FB:
      result = collCXP0FB();
      break;

    case CXP1FB:
      result = collCXP1FB();
      break;

    case CXM0FB:
      result = collCXM0FB();
      break;

    case CXM1FB:
      result = collCXM1FB();
      break;

    case CXPPMM:
      result = collCXPPMM();
      break;

    case CXBLPF:
      result = collCXBLPF();
      break;

    case INPT0:
      updatePaddle(0);
      result = myPaddleReaders[0].inpt(myTimestamp) | (lastDataBusValue & 0x40);
      break;

    case INPT1:
      updatePaddle(1);
      result = myPaddleReaders[1].inpt(myTimestamp) | (lastDataBusValue & 0x40);
      break;

    case INPT2:
      updatePaddle(2);
      result = myPaddleReaders[2].inpt(myTimestamp) | (lastDataBusValue & 0x40);
      break;

    case INPT3:
      updatePaddle(3);
      result = myPaddleReaders[3].inpt(myTimestamp) | (lastDataBusValue & 0x40);
      break;

    case INPT4:
      result =
        myInput0.inpt(!myConsole.leftController().read(Controller::Six)) |
        (lastDataBusValue & 0x40);
      break;

    case INPT5:
      result =
        myInput1.inpt(!myConsole.rightController().read(Controller::Six)) |
        (lastDataBusValue & 0x40);
      break;

    default:
      result = 0;
  }

  return (result & 0xC0) | (lastDataBusValue & 0x3F);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::poke(uInt16 address, uInt8 value)
{
  updateEmulation();

  address &= 0x3F;

  switch (address)
  {
    case WSYNC:
      mySystem->m6502().requestHalt();
      break;

    case RSYNC:
      flushLineCache();
      applyRsync();
      myShadowRegisters[address] = value;
      break;

    case VSYNC:
      myFrameManager->setVsync(value & 0x02);
      myShadowRegisters[address] = value;
      break;

    case VBLANK:
      myInput0.vblank(value);
      myInput1.vblank(value);

      for (PaddleReader& paddleReader : myPaddleReaders)
        paddleReader.vblank(value, myTimestamp);

      myDelayQueue.push(VBLANK, value, Delay::vblank);

      break;

    case AUDV0:
      myAudio.channel0().audv(value);
      myShadowRegisters[address] = value;
      break;

    case AUDV1:
      myAudio.channel1().audv(value);
      myShadowRegisters[address] = value;
      break;

    case AUDF0:
      myAudio.channel0().audf(value);
      myShadowRegisters[address] = value;
      break;

    case AUDF1:
      myAudio.channel1().audf(value);
      myShadowRegisters[address] = value;
      break;

    case AUDC0:
      myAudio.channel0().audc(value);
      myShadowRegisters[address] = value;
      break;

    case AUDC1:
      myAudio.channel1().audc(value);
      myShadowRegisters[address] = value;
      break;

    case HMOVE:
      myDelayQueue.push(HMOVE, value, Delay::hmove);
      break;

    case COLUBK:
      myBackground.setColor(value & 0xFE);
      myShadowRegisters[address] = value;
      break;

    case COLUP0:
      value &= 0xFE;
      myPlayfield.setColorP0(value);
      myMissile0.setColor(value);
      myPlayer0.setColor(value);
      myShadowRegisters[address] = value;
      break;

    case COLUP1:
      value &= 0xFE;
      myPlayfield.setColorP1(value);
      myMissile1.setColor(value);
      myPlayer1.setColor(value);
      myShadowRegisters[address] = value;
      break;

    case CTRLPF:
      flushLineCache();
      myPriority = (value & 0x04) ? Priority::pfp :
                   (value & 0x02) ? Priority::score : Priority::normal;
      myPlayfield.ctrlpf(value);
      myBall.ctrlpf(value);
      myShadowRegisters[address] = value;
      break;

    case COLUPF:
      flushLineCache();
      value &= 0xFE;
      myPlayfield.setColor(value);
      myBall.setColor(value);
      myShadowRegisters[address] = value;
      break;

    case PF0:
    {
      myDelayQueue.push(PF0, value, Delay::pf);
    #ifdef DEBUGGER_SUPPORT
      uInt16 dataAddr = mySystem->m6502().lastDataAddressForPoke();
      if(dataAddr)
        mySystem->setAccessFlags(dataAddr, CartDebug::PGFX);
    #endif
      break;
    }

    case PF1:
    {
      myDelayQueue.push(PF1, value, Delay::pf);
    #ifdef DEBUGGER_SUPPORT
      uInt16 dataAddr = mySystem->m6502().lastDataAddressForPoke();
      if(dataAddr)
        mySystem->setAccessFlags(dataAddr, CartDebug::PGFX);
    #endif
      break;
    }

    case PF2:
    {
      myDelayQueue.push(PF2, value, Delay::pf);
    #ifdef DEBUGGER_SUPPORT
      uInt16 dataAddr = mySystem->m6502().lastDataAddressForPoke();
      if(dataAddr)
        mySystem->setAccessFlags(dataAddr, CartDebug::PGFX);
    #endif
      break;
    }

    case ENAM0:
      myDelayQueue.push(ENAM0, value, Delay::enam);
      break;

    case ENAM1:
      myDelayQueue.push(ENAM1, value, Delay::enam);
      break;

    case RESM0:
      flushLineCache();
      myMissile0.resm(resxCounter(), myHstate == HState::blank);
      myShadowRegisters[address] = value;
      break;

    case RESM1:
      flushLineCache();
      myMissile1.resm(resxCounter(), myHstate == HState::blank);
      myShadowRegisters[address] = value;
      break;

    case RESMP0:
      myMissile0.resmp(value, myPlayer0);
      myShadowRegisters[address] = value;
      break;

    case RESMP1:
      myMissile1.resmp(value, myPlayer1);
      myShadowRegisters[address] = value;
      break;

    case NUSIZ0:
      flushLineCache();
      myMissile0.nusiz(value);
      myPlayer0.nusiz(value, myHstate == HState::blank);
      myShadowRegisters[address] = value;
      break;

    case NUSIZ1:
      flushLineCache();
      myMissile1.nusiz(value);
      myPlayer1.nusiz(value, myHstate == HState::blank);
      myShadowRegisters[address] = value;
      break;

    case HMM0:
      myDelayQueue.push(HMM0, value, Delay::hmm);
      break;

    case HMM1:
      myDelayQueue.push(HMM1, value, Delay::hmm);
      break;

    case HMCLR:
      myDelayQueue.push(HMCLR, value, Delay::hmclr);
      break;

    case GRP0:
    {
      myDelayQueue.push(GRP0, value, Delay::grp);
      myDelayQueue.push(DummyRegisters::shuffleP1, 0, Delay::shufflePlayer);
    #ifdef DEBUGGER_SUPPORT
      uInt16 dataAddr = mySystem->m6502().lastDataAddressForPoke();
      if(dataAddr)
        mySystem->setAccessFlags(dataAddr, CartDebug::GFX);
    #endif
      break;
    }

    case GRP1:
    {
      myDelayQueue.push(GRP1, value, Delay::grp);
      myDelayQueue.push(DummyRegisters::shuffleP0, 0, Delay::shufflePlayer);
      myDelayQueue.push(DummyRegisters::shuffleBL, 0, Delay::shuffleBall);
    #ifdef DEBUGGER_SUPPORT
      uInt16 dataAddr = mySystem->m6502().lastDataAddressForPoke();
      if(dataAddr)
        mySystem->setAccessFlags(dataAddr, CartDebug::GFX);
    #endif
      break;
    }

    case RESP0:
      flushLineCache();
      myPlayer0.resp(resxCounter());
      myShadowRegisters[address] = value;
      break;

    case RESP1:
      flushLineCache();
      myPlayer1.resp(resxCounter());
      myShadowRegisters[address] = value;
      break;

    case REFP0:
      myDelayQueue.push(REFP0, value, Delay::refp);
      break;

    case REFP1:
      myDelayQueue.push(REFP1, value, Delay::refp);
      break;

    case VDELP0:
      myPlayer0.vdelp(value);
      myShadowRegisters[address] = value;
      break;

    case VDELP1:
      myPlayer1.vdelp(value);
      myShadowRegisters[address] = value;
      break;

    case HMP0:
      myDelayQueue.push(HMP0, value, Delay::hmp);
      break;

    case HMP1:
      myDelayQueue.push(HMP1, value, Delay::hmp);
      break;

    case ENABL:
      myDelayQueue.push(ENABL, value, Delay::enabl);
      break;

    case RESBL:
      flushLineCache();
      myBall.resbl(resxCounter());
      myShadowRegisters[address] = value;
      break;

    case VDELBL:
      myBall.vdelbl(value);
      myShadowRegisters[address] = value;
      break;

    case HMBL:
      myDelayQueue.push(HMBL, value, Delay::hmbl);
      break;

    case CXCLR:
      flushLineCache();
      myCollisionMask = 0;
      myShadowRegisters[address] = value;
      break;
  }

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::saveDisplay(Serializer& out) const
{
  try
  {
    out.putByteArray(myFramebuffer, 160* TIAConstants::frameBufferHeight);
    out.putByteArray(myBackBuffer, 160 * TIAConstants::frameBufferHeight);
    out.putByteArray(myFrontBuffer, 160 * TIAConstants::frameBufferHeight);
    out.putBool(myNewFramePending);
  }
  catch(...)
  {
    cerr << "ERROR: TIA::saveDisplay" << endl;
    return false;
  }

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::loadDisplay(Serializer& in)
{
  try
  {
    // Reset frame buffer pointer and data
    in.getByteArray(myFramebuffer, 160 * TIAConstants::frameBufferHeight);
    in.getByteArray(myBackBuffer, 160 * TIAConstants::frameBufferHeight);
    in.getByteArray(myFrontBuffer, 160 * TIAConstants::frameBufferHeight);
    myNewFramePending = in.getBool();
  }
  catch(...)
  {
    cerr << "ERROR: TIA::loadDisplay" << endl;
    return false;
  }

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::update(DispatchResult& result, uInt64 maxCycles)
{
  mySystem->m6502().execute(maxCycles, result);

  updateEmulation();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::renderToFrameBuffer()
{
  if (!myNewFramePending) return;

  memcpy(myFramebuffer, myFrontBuffer, 160 * TIAConstants::frameBufferHeight);

  myFrameBufferFrameRate = myFrontBufferFrameRate;
  myFrameBufferScanlines = myFrontBufferScanlines;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::update(uInt64 maxCycles)
{
  DispatchResult dispatchResult;

  update(dispatchResult, maxCycles);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::enableColorLoss(bool enabled)
{
  bool allowColorLoss = consoleTiming() == ConsoleTiming::pal;

  if(allowColorLoss && enabled)
  {
    myColorLossEnabled = true;
    myColorLossActive = myFrameManager->scanlinesLastFrame() & 0x1;
  }
  else
  {
    myColorLossEnabled = myColorLossActive = false;

    myMissile0.applyColorLoss();
    myMissile1.applyColorLoss();
    myPlayer0.applyColorLoss();
    myPlayer1.applyColorLoss();
    myBall.applyColorLoss();
    myPlayfield.applyColorLoss();
    myBackground.applyColorLoss();
  }

  return allowColorLoss;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::electronBeamPos(uInt32& x, uInt32& y) const
{
  uInt8 clocks = clocksThisLine();

  x = (clocks < 68) ? 0 : clocks - 68;
  y = myFrameManager->getY();

  return isRendering();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::toggleBit(TIABit b, uInt8 mode)
{
  uInt8 mask;

  switch (mode) {
    case 0:
      mask = 0;
      break;

    case 1:
      mask = b;
      break;

    default:
      mask = (~mySpriteEnabledBits & b);
      break;
  }

  mySpriteEnabledBits = (mySpriteEnabledBits & ~b) | mask;

  myMissile0.toggleEnabled(mySpriteEnabledBits & TIABit::M0Bit);
  myMissile1.toggleEnabled(mySpriteEnabledBits & TIABit::M1Bit);
  myPlayer0.toggleEnabled(mySpriteEnabledBits & TIABit::P0Bit);
  myPlayer1.toggleEnabled(mySpriteEnabledBits & TIABit::P1Bit);
  myBall.toggleEnabled(mySpriteEnabledBits & TIABit::BLBit);
  myPlayfield.toggleEnabled(mySpriteEnabledBits & TIABit::PFBit);

  return mask;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::toggleBits()
{
  toggleBit(TIABit(0xFF), mySpriteEnabledBits > 0 ? 0 : 1);

  return mySpriteEnabledBits;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::toggleCollision(TIABit b, uInt8 mode)
{
  uInt8 mask;

  switch (mode) {
    case 0:
      mask = 0;
      break;

    case 1:
      mask = b;
      break;

    default:
      mask = (~myCollisionsEnabledBits & b);
      break;
  }

  myCollisionsEnabledBits = (myCollisionsEnabledBits & ~b) | mask;

  myMissile0.toggleCollisions(myCollisionsEnabledBits & TIABit::M0Bit);
  myMissile1.toggleCollisions(myCollisionsEnabledBits & TIABit::M1Bit);
  myPlayer0.toggleCollisions(myCollisionsEnabledBits & TIABit::P0Bit);
  myPlayer1.toggleCollisions(myCollisionsEnabledBits & TIABit::P1Bit);
  myBall.toggleCollisions(myCollisionsEnabledBits & TIABit::BLBit);
  myPlayfield.toggleCollisions(myCollisionsEnabledBits & TIABit::PFBit);

  return mask;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::toggleCollisions()
{
  toggleCollision(TIABit(0xFF), myCollisionsEnabledBits > 0 ? 0 : 1);

  return myCollisionsEnabledBits;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::enableFixedColors(bool enable)
{
  // This will be called during reset at a point where no frame manager
  // instance is available, so we guard aginst this here.
  int layout = 0;
  if (myFrameManager) layout = myFrameManager->layout() == FrameLayout::pal ? 1 : 0;

  myMissile0.setDebugColor(myFixedColorPalette[layout][FixedObject::M0]);
  myMissile1.setDebugColor(myFixedColorPalette[layout][FixedObject::M1]);
  myPlayer0.setDebugColor(myFixedColorPalette[layout][FixedObject::P0]);
  myPlayer1.setDebugColor(myFixedColorPalette[layout][FixedObject::P1]);
  myBall.setDebugColor(myFixedColorPalette[layout][FixedObject::BL]);
  myPlayfield.setDebugColor(myFixedColorPalette[layout][FixedObject::PF]);
  myBackground.setDebugColor(FixedColor::BK_GREY);

  myMissile0.enableDebugColors(enable);
  myMissile1.enableDebugColors(enable);
  myPlayer0.enableDebugColors(enable);
  myPlayer1.enableDebugColors(enable);
  myBall.enableDebugColors(enable);
  myPlayfield.enableDebugColors(enable);
  myBackground.enableDebugColors(enable);
  myColorHBlank = enable ? FixedColor::HBLANK_WHITE : 0x00;

  return enable;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::setFixedColorPalette(const string& colors)
{
  string s = colors;
  sort(s.begin(), s.end());
  if(s != "bgopry")
    return false;

  for(int i = 0; i < 6; ++i)
  {
    switch(colors[i])
    {
      case 'r':
        myFixedColorPalette[0][i] = FixedColor::NTSC_RED;
        myFixedColorPalette[1][i] = FixedColor::PAL_RED;
        myFixedColorNames[i] = "Red   ";
        break;
      case 'o':
        myFixedColorPalette[0][i] = FixedColor::NTSC_ORANGE;
        myFixedColorPalette[1][i] = FixedColor::PAL_ORANGE;
        myFixedColorNames[i] = "Orange";
        break;
      case 'y':
        myFixedColorPalette[0][i] = FixedColor::NTSC_YELLOW;
        myFixedColorPalette[1][i] = FixedColor::PAL_YELLOW;
        myFixedColorNames[i] = "Yellow";
        break;
      case 'g':
        myFixedColorPalette[0][i] = FixedColor::NTSC_GREEN;
        myFixedColorPalette[1][i] = FixedColor::PAL_GREEN;
        myFixedColorNames[i] = "Green ";
        break;
      case 'b':
        myFixedColorPalette[0][i] = FixedColor::NTSC_BLUE;
        myFixedColorPalette[1][i] = FixedColor::PAL_BLUE;
        myFixedColorNames[i] = "Blue  ";
        break;
      case 'p':
        myFixedColorPalette[0][i] = FixedColor::NTSC_PURPLE;
        myFixedColorPalette[1][i] = FixedColor::PAL_PURPLE;
        myFixedColorNames[i] = "Purple";
        break;
    }
  }

  // If already in fixed debug colours mode, update the current palette
  if(usingFixedColors())
    enableFixedColors(true);

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::driveUnusedPinsRandom(uInt8 mode)
{
  // If mode is 0 or 1, use it as a boolean (off or on)
  // Otherwise, return the state
  if (mode == 0 || mode == 1)
  {
    myTIAPinsDriven = bool(mode);
    mySettings.setValue(mySettings.getBool("dev.settings") ? "dev.tiadriven" : "plr.tiadriven", myTIAPinsDriven);
  }
  return myTIAPinsDriven;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool TIA::toggleJitter(uInt8 mode)
{
  switch (mode) {
    case 0:
      myEnableJitter = false;
      break;

    case 1:
      myEnableJitter = true;
      break;

    case 2:
      myEnableJitter = !myEnableJitter;
      break;

    default:
      throw runtime_error("invalid argument for toggleJitter");
  }

  if (myFrameManager) myFrameManager->enableJitter(myEnableJitter);

  return myEnableJitter;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::setJitterRecoveryFactor(Int32 factor)
{
  myJitterFactor = factor;

  if (myFrameManager) myFrameManager->setJitterFactor(myJitterFactor);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
shared_ptr<DelayQueueIterator> TIA::delayQueueIterator() const
{
  return shared_ptr<DelayQueueIterator>(
    new DelayQueueIteratorImpl<delayQueueLength, delayQueueSize>(myDelayQueue)
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TIA& TIA::updateScanline()
{
  // Update frame by one scanline at a time
  uInt32 line = scanlines();
  while (line == scanlines() && mySystem->m6502().execute(1));

  return *this;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TIA& TIA::updateScanlineByStep()
{
  // Update frame by one CPU instruction/color clock
  mySystem->m6502().execute(1);

  return *this;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TIA& TIA::updateScanlineByTrace(int target)
{
  uInt32 count = 100;  // only try up to 100 steps
  while (mySystem->m6502().getPC() != target && count-- &&
         mySystem->m6502().execute(1));

  return *this;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::registerValue(uInt8 reg) const
{
  return reg < 64 ? myShadowRegisters[reg] : 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::updateEmulation()
{
  const uInt64 systemCycles = mySystem->cycles();

  if (mySubClock > 2)
    throw runtime_error("subclock exceeds range");

  const uInt32 cyclesToRun = 3 * uInt32(systemCycles - myLastCycle) + mySubClock;

  mySubClock = 0;
  myLastCycle = systemCycles;

  cycle(cyclesToRun);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::onFrameStart()
{
  myXAtRenderingStart = 0;

  // Check for colour-loss emulation
  if (myColorLossEnabled)
  {
    // Only activate it when necessary, since changing colours in
    // the graphical object forces the TIA cached line to be flushed
    if (myFrameManager->scanlineParityChanged())
    {
      myColorLossActive = myFrameManager->scanlinesLastFrame() & 0x1;

      myMissile0.applyColorLoss();
      myMissile1.applyColorLoss();
      myPlayer0.applyColorLoss();
      myPlayer1.applyColorLoss();
      myBall.applyColorLoss();
      myPlayfield.applyColorLoss();
      myBackground.applyColorLoss();
    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::onFrameComplete()
{
  mySystem->m6502().stop();
  myCyclesAtFrameStart = mySystem->cycles();

  if (myXAtRenderingStart > 0)
    memset(myBackBuffer, 0, myXAtRenderingStart);

  // Blank out any extra lines not drawn this frame
  const Int32 missingScanlines = myFrameManager->missingScanlines();
  if (missingScanlines > 0)
    memset(myBackBuffer + 160 * myFrameManager->getY(), 0, missingScanlines * 160);

  memcpy(myFrontBuffer, myBackBuffer, 160 * TIAConstants::frameBufferHeight);

  myFrontBufferFrameRate = frameRate();
  myFrontBufferScanlines = scanlinesLastFrame();

  myNewFramePending = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::onHalt()
{
  mySubClock += (228 - myHctr) % 228;
  mySystem->incrementCycles(mySubClock / 3);
  mySubClock %= 3;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::cycle(uInt32 colorClocks)
{
  for (uInt32 i = 0; i < colorClocks; i++)
  {
    myDelayQueue.execute(
      [this] (uInt8 address, uInt8 value) {delayedWrite(address, value);}
    );

    myCollisionUpdateRequired = false;

    if (myLinesSinceChange < 2) {
      tickMovement();

      if (myHstate == HState::blank)
        tickHblank();
      else
        tickHframe();

      if (myCollisionUpdateRequired && !myFrameManager->vblank()) updateCollision();
    }

    if (++myHctr >= 228)
      nextLine();

    myAudio.tick();

    myTimestamp++;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::tickMovement()
{
  if (!myMovementInProgress) return;

  if ((myHctr & 0x03) == 0) {
    const bool apply = myHstate == HState::blank;
    bool m = false;
    uInt8 movementCounter = myMovementClock > 15 ? 0 : myMovementClock;

    m = myMissile0.movementTick(movementCounter, myHctr, apply) || m;
    m = myMissile1.movementTick(movementCounter, myHctr, apply) || m;
    m = myPlayer0.movementTick(movementCounter, apply) || m;
    m = myPlayer1.movementTick(movementCounter, apply) || m;
    m = myBall.movementTick(movementCounter, apply) || m;

    myMovementInProgress = m;
    myCollisionUpdateRequired = m;

    myMovementClock++;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::tickHblank()
{
  switch (myHctr) {
    case 0:
      myExtendedHblank = false;
      break;

    case 67:
      if (!myExtendedHblank) myHstate = HState::frame;
      break;

    case 75:
      if (myExtendedHblank) myHstate = HState::frame;
      break;
  }

  if (myExtendedHblank && myHctr > 67) myPlayfield.tick(myHctr - 68 - myHctrDelta);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::tickHframe()
{
  const uInt32 y = myFrameManager->getY();
  const uInt32 x = myHctr - 68 - myHctrDelta;

  myCollisionUpdateRequired = true;

  myPlayfield.tick(x);
  myMissile0.tick(myHctr);
  myMissile1.tick(myHctr);
  myPlayer0.tick();
  myPlayer1.tick();
  myBall.tick();

  if (myFrameManager->isRendering())
    renderPixel(x, y);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::applyRsync()
{
  const uInt32 x = myHctr > 68 ? myHctr - 68 : 0;

  myHctrDelta = 225 - myHctr;
  if (myFrameManager->isRendering())
    memset(myBackBuffer + myFrameManager->getY() * 160 + x, 0, 160 - x);

  myHctr = 225;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::nextLine()
{
  if (myLinesSinceChange >= 2) {
    cloneLastLine();
  }

  myHctr = 0;

  if (!myMovementInProgress && myLinesSinceChange < 2) myLinesSinceChange++;

  myHstate = HState::blank;
  myHctrDelta = 0;

  myFrameManager->nextLine();
  myMissile0.nextLine();
  myMissile1.nextLine();
  myPlayer0.nextLine();
  myPlayer1.nextLine();
  myBall.nextLine();
  myPlayfield.nextLine();

  if (myFrameManager->isRendering() && myFrameManager->getY() == 0) flushLineCache();

  mySystem->m6502().clearHaltRequest();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::cloneLastLine()
{
  const auto y = myFrameManager->getY();

  if (!myFrameManager->isRendering() || y == 0) return;

  uInt8* buffer = myBackBuffer;

  memcpy(buffer + y * 160, buffer + (y-1) * 160, 160);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::updateCollision()
{
  myCollisionMask |= (
    myPlayer0.collision &
    myPlayer1.collision &
    myMissile0.collision &
    myMissile1.collision &
    myBall.collision &
    myPlayfield.collision
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::renderPixel(uInt32 x, uInt32 y)
{
  if (x >= 160) return;

  uInt8 color = 0;

  if (!myFrameManager->vblank())
  {
    switch (myPriority)
    {
      case Priority::pfp:  // CTRLPF D2=1, D1=ignored
        // Playfield has priority so ScoreBit isn't used
        // Priority from highest to lowest:
        //   BL/PF => P0/M0 => P1/M1 => BK
        if (myPlayfield.isOn())       color = myPlayfield.getColor();
        else if (myBall.isOn())       color = myBall.getColor();
        else if (myPlayer0.isOn())    color = myPlayer0.getColor();
        else if (myMissile0.isOn())   color = myMissile0.getColor();
        else if (myPlayer1.isOn())    color = myPlayer1.getColor();
        else if (myMissile1.isOn())   color = myMissile1.getColor();
        else                          color = myBackground.getColor();
        break;

      case Priority::score:  // CTRLPF D2=0, D1=1
        // Formally we have (priority from highest to lowest)
        //   PF/P0/M0 => P1/M1 => BL => BK
        // for the first half and
        //   P0/M0 => PF/P1/M1 => BL => BK
        // for the second half. However, the first ordering is equivalent
        // to the second (PF has the same color as P0/M0), so we can just
        // write
        if (myPlayer0.isOn())         color = myPlayer0.getColor();
        else if (myMissile0.isOn())   color = myMissile0.getColor();
        else if (myPlayfield.isOn())  color = myPlayfield.getColor();
        else if (myPlayer1.isOn())    color = myPlayer1.getColor();
        else if (myMissile1.isOn())   color = myMissile1.getColor();
        else if (myBall.isOn())       color = myBall.getColor();
        else                          color = myBackground.getColor();
        break;

      case Priority::normal:  // CTRLPF D2=0, D1=0
        // Priority from highest to lowest:
        //   P0/M0 => P1/M1 => BL/PF => BK
        if (myPlayer0.isOn())         color = myPlayer0.getColor();
        else if (myMissile0.isOn())   color = myMissile0.getColor();
        else if (myPlayer1.isOn())    color = myPlayer1.getColor();
        else if (myMissile1.isOn())   color = myMissile1.getColor();
        else if (myPlayfield.isOn())  color = myPlayfield.getColor();
        else if (myBall.isOn())       color = myBall.getColor();
        else                          color = myBackground.getColor();
        break;
    }
  }

  myBackBuffer[y * 160 + x] = color;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::flushLineCache()
{
  const bool wasCaching = myLinesSinceChange >= 2;

  myLinesSinceChange = 0;

  if (wasCaching) {
    const auto rewindCycles = myHctr;

    for (myHctr = 0; myHctr < rewindCycles; myHctr++) {
      if (myHstate == HState::blank)
        tickHblank();
      else
        tickHframe();
    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::clearHmoveComb()
{
  if (myFrameManager->isRendering() && myHstate == HState::blank)
    memset(myBackBuffer + myFrameManager->getY() * 160, myColorHBlank, 8);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::delayedWrite(uInt8 address, uInt8 value)
{
  if (address < 64)
    myShadowRegisters[address] = value;

  switch (address)
  {
    case VBLANK:
      flushLineCache();
      myFrameManager->setVblank(value & 0x02);
      break;

    case HMOVE:
      flushLineCache();

      myMovementClock = 0;
      myMovementInProgress = true;

      if (!myExtendedHblank) {
        clearHmoveComb();
        myExtendedHblank = true;
      }

      myMissile0.startMovement();
      myMissile1.startMovement();
      myPlayer0.startMovement();
      myPlayer1.startMovement();
      myBall.startMovement();
      break;

    case PF0:
      myPlayfield.pf0(value);
      break;

    case PF1:
      myPlayfield.pf1(value);
      break;

    case PF2:
      myPlayfield.pf2(value);
      break;

    case HMM0:
      myMissile0.hmm(value);
      break;

    case HMM1:
      myMissile1.hmm(value);
      break;

    case HMCLR:
      // We must update the shadow registers for each HM object too
      myMissile0.hmm(0);  myShadowRegisters[HMM0] = 0;
      myMissile1.hmm(0);  myShadowRegisters[HMM1] = 0;
      myPlayer0.hmp(0);   myShadowRegisters[HMP0] = 0;
      myPlayer1.hmp(0);   myShadowRegisters[HMP1] = 0;
      myBall.hmbl(0);     myShadowRegisters[HMBL] = 0;
      break;

    case GRP0:
      myPlayer0.grp(value);
      break;

    case GRP1:
      myPlayer1.grp(value);
      break;

    case DummyRegisters::shuffleP0:
      myPlayer0.shufflePatterns();
      break;

    case DummyRegisters::shuffleP1:
      myPlayer1.shufflePatterns();
      break;

    case DummyRegisters::shuffleBL:
      myBall.shuffleStatus();
      break;

    case HMP0:
      myPlayer0.hmp(value);
      break;

    case HMP1:
      myPlayer1.hmp(value);
      break;

    case HMBL:
      myBall.hmbl(value);
      break;

    case REFP0:
      myPlayer0.refp(value);
      break;

    case REFP1:
      myPlayer1.refp(value);
      break;

    case ENABL:
      myBall.enabl(value);
      break;

    case ENAM0:
      myMissile0.enam(value);
      break;

    case ENAM1:
      myMissile1.enam(value);
      break;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::updatePaddle(uInt8 idx)
{
  Int32 resistance;
  switch (idx) {
    case 0:
      resistance = myConsole.leftController().read(Controller::Nine);
      break;

    case 1:
      resistance = myConsole.leftController().read(Controller::Five);
      break;

    case 2:
      resistance = myConsole.rightController().read(Controller::Nine);
      break;

    case 3:
      resistance = myConsole.rightController().read(Controller::Five);
      break;

    default:
      throw runtime_error("invalid paddle index");
  }

  myPaddleReaders[idx].update(
    (resistance == Controller::MAX_RESISTANCE) ? -1 : (double(resistance) / Paddles::MAX_RESISTANCE),
    myTimestamp,
    consoleTiming()
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::resxCounter()
{
  return myHstate == HState::blank ?
    (myHctr >= resxLateHblankThreshold ? ResxCounter::lateHblank : ResxCounter::hblank) : ResxCounter::frame;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::collCXM0P() const
{
  return (
    ((myCollisionMask & CollisionMask::missile0 & CollisionMask::player0) ? 0x40 : 0) |
    ((myCollisionMask & CollisionMask::missile0 & CollisionMask::player1) ? 0x80 : 0)
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::collCXM1P() const
{
  return (
    ((myCollisionMask & CollisionMask::missile1 & CollisionMask::player1) ? 0x40 : 0) |
    ((myCollisionMask & CollisionMask::missile1 & CollisionMask::player0) ? 0x80 : 0)
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::collCXP0FB() const
{
  return (
    ((myCollisionMask & CollisionMask::player0 & CollisionMask::ball) ? 0x40 : 0) |
    ((myCollisionMask & CollisionMask::player0 & CollisionMask::playfield) ? 0x80 : 0)
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::collCXP1FB() const
{
  return (
    ((myCollisionMask & CollisionMask::player1 & CollisionMask::ball) ? 0x40 : 0) |
    ((myCollisionMask & CollisionMask::player1 & CollisionMask::playfield) ? 0x80 : 0)
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::collCXM0FB() const
{
  return (
    ((myCollisionMask & CollisionMask::missile0 & CollisionMask::ball) ? 0x40 : 0) |
    ((myCollisionMask & CollisionMask::missile0 & CollisionMask::playfield) ? 0x80 : 0)
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::collCXM1FB() const
{
  return (
    ((myCollisionMask & CollisionMask::missile1 & CollisionMask::ball) ? 0x40 : 0) |
    ((myCollisionMask & CollisionMask::missile1 & CollisionMask::playfield) ? 0x80 : 0)
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::collCXPPMM() const
{
  return (
    ((myCollisionMask & CollisionMask::missile0 & CollisionMask::missile1) ? 0x40 : 0) |
    ((myCollisionMask & CollisionMask::player0 & CollisionMask::player1) ? 0x80 : 0)
  );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::collCXBLPF() const
{
  return (myCollisionMask & CollisionMask::ball & CollisionMask::playfield) ? 0x80 : 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollP0PF()
{
  myCollisionMask ^= (CollisionMask::player0 & CollisionMask::playfield);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollP0BL()
{
  myCollisionMask ^= (CollisionMask::player0 & CollisionMask::ball);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollP0M1()
{
  myCollisionMask ^= (CollisionMask::player0 & CollisionMask::missile1);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollP0M0()
{
  myCollisionMask ^= (CollisionMask::player0 & CollisionMask::missile0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollP0P1()
{
  myCollisionMask ^= (CollisionMask::player0 & CollisionMask::player1);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollP1PF()
{
  myCollisionMask ^= (CollisionMask::player1 & CollisionMask::playfield);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollP1BL()
{
  myCollisionMask ^= (CollisionMask::player1 & CollisionMask::ball);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollP1M1()
{
  myCollisionMask ^= (CollisionMask::player1 & CollisionMask::missile1);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollP1M0()
{
  myCollisionMask ^= (CollisionMask::player1 & CollisionMask::missile0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollM0PF()
{
  myCollisionMask ^= (CollisionMask::missile0 & CollisionMask::playfield);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollM0BL()
{
  myCollisionMask ^= (CollisionMask::missile0 & CollisionMask::ball);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollM0M1()
{
  myCollisionMask ^= (CollisionMask::missile0 & CollisionMask::missile1);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollM1PF()
{
  myCollisionMask ^= (CollisionMask::missile1 & CollisionMask::playfield);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollM1BL()
{
  myCollisionMask ^= (CollisionMask::missile1 & CollisionMask::ball);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::toggleCollBLPF()
{
  myCollisionMask ^= (CollisionMask::ball & CollisionMask::playfield);
}

#ifdef DEBUGGER_SUPPORT
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::createAccessBase()
{
  myAccessBase = make_unique<uInt8[]>(TIA_SIZE);
  memset(myAccessBase.get(), CartDebug::NONE, TIA_SIZE);
  myAccessDelay = make_unique<uInt8[]>(TIA_SIZE);
  memset(myAccessDelay.get(), TIA_DELAY, TIA_SIZE);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uInt8 TIA::getAccessFlags(uInt16 address) const
{
  return myAccessBase[address & TIA_MASK];
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void TIA::setAccessFlags(uInt16 address, uInt8 flags)
{
  // ignore none flag
  if (flags != CartDebug::NONE) {
    if (flags == CartDebug::WRITE) {
      // the first two write accesses are assumed as initialization
      if (myAccessDelay[address & TIA_MASK])
        myAccessDelay[address & TIA_MASK]--;
      else
        myAccessBase[address & TIA_MASK] |= flags;
    } else
      myAccessBase[address & TIA_READ_MASK] |= flags;
  }
}
#endif // DEBUGGER_SUPPORT
