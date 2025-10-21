/*
 * InputDevice Header
 * Handles SDL input device management
 */

#ifndef INPUTDEVICE_HPP
#define INPUTDEVICE_HPP

#include <SDL3/SDL.h>
#include <SDL3/SDL_oldnames.h>
#include <string>

namespace Thread
{
    class SDLThread;
}

namespace Utilities
{

class InputDevice
{
public:
    InputDevice();
    ~InputDevice();

    void OpenDevice(const std::string& name, const std::string& path, const std::string& serial, int number);
    void CloseDevice();
    
    bool HasOpenDevice() const;
    bool IsAttached() const;
    bool IsOpeningDevice() const;
    
    SDL_Gamepad* GetGameControllerHandle() const;
    SDL_Joystick* GetJoystickHandle() const;
    
    void StartRumble();
    void StopRumble();
    
    void SetSDLThread(Thread::SDLThread* thread);

private:
    SDL_Gamepad* gameController;
    SDL_Joystick* joystick;
    Thread::SDLThread* sdlThread;
    bool isOpening;
    std::string deviceName;
    std::string devicePath;
    std::string deviceSerial;
    int deviceNumber;
};

} // namespace Utilities

#endif // INPUTDEVICE_HPP
