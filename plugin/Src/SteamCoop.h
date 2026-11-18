#pragma once

#include "coop/Socket.h"

// Uses the Steam API already initialized by the Steam edition of HITMAN.
class SteamCoop {
public:
    enum class Transfer { None, Host, Join };

    struct Ready {
        Transfer mode = Transfer::None;
        std::unique_ptr<coop::IDatagramSocket> socket;
        coop::Endpoint host;
    };

    SteamCoop();
    ~SteamCoop();

    bool Available() const;
    bool Pending() const;
    bool Hosting() const;
    void Host();
    void Cancel();
    void Invite();
    Ready Update(bool p_CanJoin);
    std::string TakeNotice();

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
