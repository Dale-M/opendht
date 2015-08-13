/*
 *  Copyright (C) 2014-2015 Savoir-Faire Linux Inc.
 *  Author : Adrien Béraud <adrien.beraud@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *  Additional permission under GNU GPL version 3 section 7:
 *
 *  If you modify this program, or any covered work, by linking or
 *  combining it with the OpenSSL project's OpenSSL library (or a
 *  modified version of that library), containing parts covered by the
 *  terms of the OpenSSL or SSLeay licenses, Savoir-Faire Linux Inc.
 *  grants you additional permission to convey the resulting work.
 *  Corresponding Source for a non-source form of such a combination
 *  shall include the source code for the parts of OpenSSL used as well
 *  as that of the covered work.
 */

#pragma once

#include "value.h"

namespace dht {

struct DhtMessage : public ValueSerializable<DhtMessage>
{
    DhtMessage(std::string s = {}, Blob msg = {}) : service(s), data(msg) {}
    
    std::string getService() const {
        return service;
    }

    static const ValueType TYPE;
    virtual const ValueType& getType() const {
        return TYPE;
    }
    static Value::Filter getFilter() { return {}; }

    static bool storePolicy(InfoHash, std::shared_ptr<Value>&, InfoHash, const sockaddr*, socklen_t);

    static Value::Filter ServiceFilter(std::string s);

    /** print value for debugging */
    friend std::ostream& operator<< (std::ostream&, const DhtMessage&);

public:
    std::string service;
    Blob data;
    MSGPACK_DEFINE(service, data);
};

template <typename Type>
struct SignedValue : public ValueSerializable<Type>
{
    virtual void unpackValue(const Value& v) {
        from = v.owner.getId();
        ValueSerializable<Type>::unpackValue(v);
    }
    static Value::Filter getFilter() {
        return [](const Value& v){ return v.isSigned(); };
    }
public:
    dht::InfoHash from;
};

template <typename Type>
struct EncryptedValue : public SignedValue<Type>
{
    virtual void unpackValue(const Value& v) {
        to = v.recipient;
        SignedValue<Type>::unpackValue(v);
    }
    static Value::Filter getFilter() {
        return Value::Filter::chain(
            SignedValue<Type>::getFilter(),
            [](const Value& v){ return v.recipient != InfoHash(); }
        );
    }

public:
    dht::InfoHash to;
};

struct ImMessage : public SignedValue<ImMessage>
{
    ImMessage() {}
    ImMessage(std::string&& msg)
      : sent(std::chrono::system_clock::now()), im_message(std::move(msg)) {}

    static const ValueType TYPE;
    virtual const ValueType& getType() const {
        return TYPE;
    }
    static Value::Filter getFilter() {
        return SignedValue::getFilter();
    }
    virtual void unpackValue(const Value& v) {
        to = v.recipient;
        SignedValue::unpackValue(v);
    }

    dht::InfoHash to;
    std::chrono::system_clock::time_point sent;
    std::string im_message;
    MSGPACK_DEFINE(im_message);
};

struct TrustRequest : public EncryptedValue<TrustRequest>
{
    TrustRequest() {}
    TrustRequest(std::string s) : service(s) {}
    TrustRequest(std::string s, const Blob& d) : service(s), payload(d) {}

    static const ValueType TYPE;
    virtual const ValueType& getType() const {
        return TYPE;
    }
    static Value::Filter getFilter() {
        return EncryptedValue::getFilter();
    }

    std::string service;
    Blob payload;
    MSGPACK_DEFINE(service, payload);
};

struct IceCandidates : public EncryptedValue<IceCandidates>
{
    IceCandidates() {}
    IceCandidates(Value::Id msg_id, Blob ice) : id(msg_id), ice_data(ice) {}

    static const ValueType TYPE;
    virtual const ValueType& getType() const {
        return TYPE;
    }
    static Value::Filter getFilter() {
        return EncryptedValue::getFilter();
    }

    Value::Id id;
    Blob ice_data;
    MSGPACK_DEFINE(id, ice_data);
};


/* "Peer" announcement
 */
struct IpServiceAnnouncement : public ValueSerializable<IpServiceAnnouncement>
{
    IpServiceAnnouncement(in_port_t p = 0) {
        ss.ss_family = 0;
        setPort(p);
    }

    IpServiceAnnouncement(const sockaddr* sa, socklen_t sa_len) {
        if (sa)
            std::copy_n((const uint8_t*)sa, sa_len, (uint8_t*)&ss);
    }

    IpServiceAnnouncement(const Blob& b) {
        msgpack_unpack(unpack(b).get());
    }

    template <typename Packer>
    void msgpack_pack(Packer& pk) const
    {
        pk.pack_array(2);
        pk.pack(getPort());
        if (ss.ss_family == AF_INET) {
            pk.pack_bin(sizeof(in_addr));
            pk.pack_bin_body((const char*)&reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr, sizeof(in_addr));
        } else if (ss.ss_family == AF_INET6) {
            pk.pack_bin(sizeof(in6_addr));
            pk.pack_bin_body((const char*)&reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr, sizeof(in6_addr));
        }
    }

    void msgpack_unpack(msgpack::object o)
    {
        if (o.type != msgpack::type::ARRAY) throw msgpack::type_error();
        if (o.via.array.size < 2) throw msgpack::type_error();
        setPort(o.via.array.ptr[0].as<in_port_t>());
        auto ip_dat = o.via.array.ptr[1].as<Blob>();
        if (ip_dat.size() == sizeof(in_addr))
            std::copy(ip_dat.begin(), ip_dat.end(), (char*)&reinterpret_cast<sockaddr_in*>(&ss)->sin_addr);
        else if (ip_dat.size() == sizeof(in6_addr))
            std::copy(ip_dat.begin(), ip_dat.end(), (char*)&reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr);
        else
            throw msgpack::type_error();
    }

    in_port_t getPort() const {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
    }
    void setPort(in_port_t p) {
        reinterpret_cast<sockaddr_in*>(&ss)->sin_port = htons(p);
    }

    sockaddr_storage getPeerAddr() const {
        return ss;
    }

    static const ValueType TYPE;
    virtual const ValueType& getType() const {
        return TYPE;
    }

    static bool storePolicy(InfoHash, std::shared_ptr<Value>&, InfoHash, const sockaddr*, socklen_t);

    /** print value for debugging */
    friend std::ostream& operator<< (std::ostream&, const IpServiceAnnouncement&);

private:
    sockaddr_storage ss;
};


const std::array<std::reference_wrapper<const ValueType>, 5>
DEFAULT_TYPES
{
    ValueType::USER_DATA,
    DhtMessage::TYPE,
    ImMessage::TYPE,
    IceCandidates::TYPE,
    TrustRequest::TYPE
};

const std::array<std::reference_wrapper<const ValueType>, 1>
DEFAULT_INSECURE_TYPES
{
    IpServiceAnnouncement::TYPE
};

}
