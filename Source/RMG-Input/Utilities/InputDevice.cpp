/*
 * InputDevice Implementation
 * Handles SDL input device management
 */

#include "InputDevice.hpp"
#include "../Thread/SDLThread.hpp"

namespace Utilities
{

InputDevice::InputDevice()
    : gameController(nullptr)
    , joystick(nullptr)
    , sdlThread(nullptr)
    , isOpening(false)
    , deviceNumber(-1)
{
}

InputDevice::~InputDevice()
{
    CloseDevice();
}

void InputDevice::OpenDevice(const std::string& name, const std::string& path, const std::string& serial, int number)
{
    CloseDevice();
    
    deviceName = name;
    devicePath = path;
    deviceSerial = serial;
    deviceNumber = number;
    isOpening = true;
    
    // Try to open as game controller first
    if (number >= 0)
    {
        gameController = SDL_OpenGamepad(number);
        if (gameController)
        {
            joystick = SDL_GetGamepadJoystick(gameController);
        }
        else
        {
            // Fallback to joystick
            joystick = SDL_OpenJoystick(number);
        }
    }
    
    isOpening = false;
}

void InputDevice::CloseDevice()
{
    if (gameController)
    {
        SDL_CloseGamepad(gameController);
        gameController = nullptr;
    }
    
    if (joystick)
    {
        SDL_CloseJoystick(joystick);
        joystick = nullptr;
    }
    
    isOpening = false;
}

bool InputDevice::HasOpenDevice() const
{
    return gameController != nullptr || joystick != nullptr;
}

bool InputDevice::IsAttached() const
{
    if (gameController)
    {
        return SDL_GamepadConnected(gameController) == true;
    }
    else if (joystick)
    {
        return SDL_JoystickConnected(joystick) == true;
    }
    
    return false;
}

bool InputDevice::IsOpeningDevice() const
{
    return isOpening;
}

SDL_Gamepad* InputDevice::GetGameControllerHandle() const
{
    return gameController;
}

SDL_Joystick* InputDevice::GetJoystickHandle() const
{
    return joystick;
}

void InputDevice::StartRumble()
{
    if (gameController)
    {
        SDL_RumbleGamepad(gameController, 0xFFFF, 0xFFFF, 1000);
    }
}

void InputDevice::StopRumble()
{
    if (gameController)
    {
        SDL_RumbleGamepad(gameController, 0, 0, 0);
    }
}

void InputDevice::SetSDLThread(Thread::SDLThread* thread)
{
    sdlThread = thread;
}

} // namespace Utilities
