/*
 * Copyright (C) 2006-2018 Istituto Italiano di Tecnologia (IIT)
 * Copyright (C) 2006-2010 RobotCub Consortium
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms of the
 * BSD-3-Clause license. See the accompanying LICENSE file for details.
 */

#include <yarp/os/Network.h>

#include <yarp/os/Bottle.h>
#include <yarp/os/Carriers.h>
#include <string>
#include <yarp/os/DummyConnector.h>
#include <yarp/os/InputStream.h>
#include <yarp/os/MultiNameSpace.h>
#include <yarp/os/NameSpace.h>
#include <yarp/os/OutputProtocol.h>
#include <yarp/os/Port.h>
#include <yarp/os/Route.h>
#include <yarp/os/Time.h>
#include <yarp/os/Thread.h>
#include <yarp/os/Vocab.h>
#include <yarp/os/YarpPlugin.h>
#include <yarp/os/Face.h>

#include <yarp/os/impl/BottleImpl.h>
#include <yarp/os/impl/BufferedConnectionWriter.h>
#include <yarp/os/impl/Companion.h>
#include <yarp/os/impl/Logger.h>
#include <yarp/os/impl/NameClient.h>
#include <yarp/os/impl/NameConfig.h>
#include <yarp/os/impl/PlatformSignal.h>
#include <yarp/os/impl/PlatformStdlib.h>
#include <yarp/os/impl/PlatformStdio.h>
#include <yarp/os/impl/PortCommand.h>
#include <yarp/os/impl/StreamConnectionReader.h>
#include <yarp/os/impl/ThreadImpl.h>
#include <yarp/os/impl/TimeImpl.h>

#ifdef YARP_HAS_ACE
# include <ace/config.h>
# include <ace/Init_ACE.h>
// In one the ACE headers there is a definition of "main" for WIN32
# ifdef main
#  undef main
# endif
#endif

#include <cstdio>
#include <cstdlib>

using namespace yarp::os::impl;
using namespace yarp::os;

static int __yarp_is_initialized = 0;
static bool __yarp_auto_init_active = false; // was yarp auto-initialized?

static MultiNameSpace *__multi_name_space = nullptr;

/**
 *
 * A single-use class to shut down the yarp library if it was
 * initialized automatically.
 *
 */
class YarpAutoInit {
public:
    /**
     *
     * Shut down the yarp library if it was automatically initialized.
     * The library is automatically initialized if
     * NetworkBase::autoInitMinimum() is called before any of the
     * manual ways of initializing the library (calling Network::init,
     * creating a Network object, etc).  yarp::os::ResourceFinder
     * calls autoInitMinimum() since it needs to be sure that
     * YARP+ACE is initialized (but a user might not expect that).
     *
     */
    ~YarpAutoInit() {
        if (__yarp_auto_init_active) {
            NetworkBase::finiMinimum();
            __yarp_auto_init_active = false;
        }
    }
};
static YarpAutoInit yarp_auto_init; ///< destructor is called on shutdown.

static MultiNameSpace& getNameSpace()
{
    if (__multi_name_space == nullptr) {
        __multi_name_space = new MultiNameSpace;
        yAssert(__multi_name_space != nullptr);
    }
    return *__multi_name_space;
}

static void removeNameSpace()
{
    if (__multi_name_space != nullptr) {
        delete __multi_name_space;
        __multi_name_space = nullptr;
    }
}

static bool needsLookup(const Contact& contact)
{
    if (contact.getHost() != "") {
        return false;
    }
    if (contact.getCarrier() == "topic") {
        return false;
    }
    return true;
}

static int noteDud(const Contact& src)
{
    NameStore *store = getNameSpace().getQueryBypass();
    if (store != nullptr) {
        return store->announce(src.getName().c_str(), 0);
    }
    Bottle cmd, reply;
    cmd.addString("announce");
    cmd.addString(src.getName().c_str());
    cmd.addInt32(0);
    ContactStyle style;
    bool ok = NetworkBase::writeToNameServer(cmd,
                                             reply,
                                             style);
    return ok ? 0 : 1;
 }


//#define DEBUG_CONNECT_CARRIER
#ifdef DEBUG_CONNECT_CARRIER
# define CARRIER_DEBUG(fmt, ...)    fprintf(stderr, fmt, ##__VA_ARGS__)
#else
# define CARRIER_DEBUG(fmt, ...)
#endif

static int enactConnection(const Contact& src,
                           const Contact& dest,
                           const ContactStyle& style,
                           int mode,
                           bool reversed)
{
    ContactStyle rpc;
    rpc.admin = true;
    rpc.quiet = style.quiet;
    rpc.timeout = style.timeout;

    CARRIER_DEBUG("enactConnection: SRC %s DST %s using carrier %s, MODE=%d, rev=%d\n",
            src.getName().c_str(), dest.getName().c_str(), style.carrier.c_str(), mode, reversed);

    if (style.persistent) {
        bool ok = false;
        // we don't talk to the ports, we talk to the nameserver
        NameSpace& ns = getNameSpace();
        if (mode==YARP_ENACT_CONNECT) {
            ok = ns.connectPortToPortPersistently(src, dest, style);
        } else if (mode==YARP_ENACT_DISCONNECT) {
            ok = ns.disconnectPortToPortPersistently(src, dest, style);
        } else {
            fprintf(stderr, "Failure: cannot check subscriptions yet\n");
            return 1;
        }
        if (!ok) {
            return 1;
        }
        if (!style.quiet) {
            fprintf(stderr, "Success: port-to-port persistent connection added.\n");
        }
        return 0;
    }

    Bottle cmd, reply;
    cmd.addVocab(Vocab::encode("list"));
    cmd.addVocab(Vocab::encode(reversed?"in":"out"));
    cmd.addString(dest.getName().c_str());
    YARP_SPRINTF2(Logger::get(), debug, "asking %s: %s",
                    src.toString().c_str(), cmd.toString().c_str());
    bool ok = NetworkBase::write(src, cmd, reply, rpc);
    if (!ok) {
        noteDud(src);
        return 1;
    }
    if (reply.check("carrier")) {
        std::string carrier = reply.find("carrier").asString();
        if (!style.quiet) {
            printf("Connection found between %s and %s using carrier %s\n",
                    src.getName().c_str(), dest.getName().c_str(), carrier.c_str());
        }
        if (mode==YARP_ENACT_EXISTS) {
            return (carrier == style.carrier) ? 0 : 1;
        }

        // This is either a connect or a disconnect command, but the current
        // connection is connectionless, the other side will not know that we
        // are closing the connection and therefore will continue sending data.
        // Therefore we send an explicit disconnect here.
        bool currentIsConnectionLess = false;
        bool currentIsPush = true;
        if (reply.check("push")) {
            currentIsPush = reply.find("push").asBool();
        }
        if (reply.check("connectionless")) {
            currentIsConnectionLess = reply.find("connectionless").asBool();
        }
        if (currentIsConnectionLess && ((reversed && currentIsPush) || (!reversed && !currentIsPush))) {
            enactConnection(dest, src, style, YARP_ENACT_DISCONNECT, !reversed);
        }
    }
    if (mode==YARP_ENACT_EXISTS) {
        return 1;
    }

    int act = (mode==YARP_ENACT_DISCONNECT)?VOCAB3('d', 'e', 'l'):VOCAB3('a', 'd', 'd');

    // Let's ask the destination to connect/disconnect to the source.
    // We assume the YARP carrier will reverse the connection if
    // appropriate when connecting.
    cmd.clear();
    reply.clear();
    cmd.addVocab(act);
    Contact c = dest;
    if (style.carrier!="") {
        c.setCarrier(style.carrier);
    }
    if (mode!=YARP_ENACT_DISCONNECT) {
        cmd.addString(c.toString());
    } else {
        cmd.addString(c.getName());
    }

    Contact c2 = src;
    if (c2.getPort()<=0) {
        c2 = NetworkBase::queryName(c2.getName());
    }

    YARP_SPRINTF2(Logger::get(), debug, "** asking %s: %s",
                  src.toString().c_str(), cmd.toString().c_str());
    ok = NetworkBase::write(c2, cmd, reply, rpc);
    if (!ok) {
        noteDud(src);
        return 1;
    }
    std::string msg = "";
    if (reply.get(0).isInt32()) {
        ok = (reply.get(0).asInt32()==0);
        msg = reply.get(1).asString();
    } else {
        // older protocol
        msg = reply.get(0).asString();
        ok = msg[0]=='A'||msg[0]=='R';
    }
    if (mode==YARP_ENACT_DISCONNECT && !ok) {
        msg = "no such connection\n";
    }
    if (mode==YARP_ENACT_CONNECT && !ok) {
        noteDud(dest);
    }
    if (!style.quiet) {
        if (style.verboseOnSuccess||!ok) {
            fprintf(stderr, "%s %s",
                    ok?"Success:":"Failure:",
                    msg.c_str());
        }
    }
    return ok ? 0 : 1;
}



static char* findCarrierParamsPointer(std::string &carrier_name)
{
    size_t i = carrier_name.find('+');
    if (i!=std::string::npos) {
        return &(carrier_name[i]);
    }
    else
        return nullptr;
}

static std::string collectParams(Contact &c)
{

    std::string carrier_name = c.getCarrier();
    char *params_ptr = findCarrierParamsPointer(carrier_name);
    std::string params;
    params.clear();

    if(nullptr != params_ptr)
    {
        params+=params_ptr;
    }

    CARRIER_DEBUG("\n ***** SONO NELLA COLLECTPARAMS: carrier=%s, params=%s\n\n ", c.getCarrier().c_str(), params.c_str());
    return params;

}

static std::string extractCarrierNameOnly(std::string &carrier_name_with_params)
{

    std::string carrier_name = carrier_name_with_params;
    char *c = findCarrierParamsPointer(carrier_name);
    if(nullptr != c){
        *c = '\0';
        carrier_name = carrier_name.c_str();
    }
    return carrier_name;
}

/*

   Connect two ports, bearing in mind that one of them may not be
   a regular YARP port.

   Normally, YARP sends a request to the source port asking it to
   connect to the destination port.  But the source port may not
   be capable of initiating connections, in which case we can
   request the destination port to connect to the source (this
   is appropriate for carriers that can reverse the initiative).

   The source or destination could also be topic ports, which are
   entirely virtual.  In that case, we just need to tell the name
   server, and it will take care of the details.

*/

static int metaConnect(const std::string& src,
                       const std::string& dest,
                       ContactStyle style,
                       int mode) {
    YARP_SPRINTF3(Logger::get(), debug,
                  "working on connection %s to %s (%s)",
                  src.c_str(),
                  dest.c_str(),
                  (mode==YARP_ENACT_CONNECT)?"connect":((mode==YARP_ENACT_DISCONNECT)?"disconnect":"check")
                  );
    // check if source name and destination name contain spaces
    if(dest.find(" ") != std::string::npos || src.find(" ") != std::string::npos)
    {
        fprintf(stderr, "Failure: no way to make connection %s->%s,\n", src.c_str(), dest.c_str());
        return 1;
    }

    CARRIER_DEBUG("METACONNECT: src=%s dest=%s style=%s\n", src.c_str(), dest.c_str(), style.carrier.c_str());
    // get the expressed contacts, without name server input
    Contact dynamicSrc = Contact::fromString(src);
    Contact dynamicDest = Contact::fromString(dest);

    CARRIER_DEBUG("DYNAMIC_SRC: name=%s, carrier=%s\n", dynamicSrc.getName().c_str(), dynamicSrc.getCarrier().c_str());
    CARRIER_DEBUG("DYNAMIC_DST: name=%s, carrier=%s\n", dynamicDest.getName().c_str(), dynamicDest.getCarrier().c_str());

    if(!NetworkBase::isValidPortName(dynamicSrc.getName()))
    {
        fprintf(stderr, "Failure: no way to make connection, invalid source '%s'\n", dynamicSrc.getName().c_str());
        return 1;
    }
    if(!NetworkBase::isValidPortName(dynamicDest.getName()))
    {
        fprintf(stderr, "Failure: no way to make connection, invalid destination '%s'\n", dynamicDest.getName().c_str());
        return 1;
    }

    bool topical = style.persistent;
    if (dynamicSrc.getCarrier()=="topic" ||
        dynamicDest.getCarrier()=="topic") {
        topical = true;
    }

    bool topicalNeedsLookup = !getNameSpace().connectionHasNameOfEndpoints();

    // fetch completed contacts from name server, if needed
    Contact staticSrc;
    Contact staticDest;
    if (needsLookup(dynamicSrc)&&(topicalNeedsLookup||!topical)) {
        staticSrc = NetworkBase::queryName(dynamicSrc.getName());
        if (!staticSrc.isValid()) {
            if (!style.persistent) {
                if (!style.quiet) {
                    fprintf(stderr, "Failure: could not find source port %s\n",
                            src.c_str());
                }
                return 1;
            } else {
                staticSrc = dynamicSrc;
            }
        }
    } else {
        staticSrc = dynamicSrc;
    }
    if (staticSrc.getCarrier()=="") {
        staticSrc.setCarrier("tcp");
    }
    if (staticDest.getCarrier()=="") {
        staticDest.setCarrier("tcp");
    }

    if (needsLookup(dynamicDest)&&(topicalNeedsLookup||!topical)) {
        staticDest = NetworkBase::queryName(dynamicDest.getName());
        if (!staticDest.isValid()) {
            if (!style.persistent) {
                if (!style.quiet) {
                    fprintf(stderr, "Failure: could not find destination port %s\n",
                            dest.c_str());
                }
                return 1;
            } else {
                staticDest = dynamicDest;
            }
        }
    } else {
        staticDest = dynamicDest;
    }

    CARRIER_DEBUG("STATIC_SRC: name=%s, carrier=%s\n", staticSrc.getName().c_str(), staticSrc.getCarrier().c_str());
    CARRIER_DEBUG("STATIC_DST: name=%s, carrier=%s\n", staticDest.getName().c_str(), staticDest.getCarrier().c_str());

    //DynamicSrc and DynamicDst are the contacts created by connect command
    //while staticSrc and staticDest are contacts created by querying th server

    if (staticSrc.getCarrier()=="xmlrpc" &&
        (staticDest.getCarrier()=="xmlrpc"||(staticDest.getCarrier().find("rossrv")==0))&&
        mode==YARP_ENACT_CONNECT) {
        // Unconnectable in general
        // Let's assume the first part is a YARP port, and use "tcp" instead
        staticSrc.setCarrier("tcp");
        staticDest.setCarrier("tcp");
    }

    std::string carrierConstraint = "";

    // see if we can do business with the source port
    bool srcIsCompetent = false;
    bool srcIsTopic = false;
    if (staticSrc.getCarrier()!="topic") {
        if (!topical) {
            Carrier *srcCarrier = nullptr;
            CARRIER_DEBUG("staticSrc.getCarrier= %s  ", staticSrc.getCarrier().c_str());
            if (staticSrc.getCarrier()!="") {
                srcCarrier = Carriers::chooseCarrier(staticSrc.getCarrier().c_str());
            }
            if (srcCarrier!=nullptr) {
                CARRIER_DEBUG("srcCarrier is NOT null; its name is %s;  ", srcCarrier->getName().c_str());
                std::string srcBootstrap = srcCarrier->getBootstrapCarrierName();
                if (srcBootstrap!="") {

                    CARRIER_DEBUG(" it is competent(bootstrapname is %s), while its name is %s )\n\n", srcBootstrap.c_str(), srcCarrier->getName().c_str());
                    srcIsCompetent = true;
                } else {
                    //if the srcCarrier is not competent, (that is it can't perform the starting yarp handshaking)
                    //set the carrier contraint equal to the carrier with which the posrt had been registered.
                    carrierConstraint = staticSrc.getCarrier();
                    CARRIER_DEBUG(" it is NOT competent. its constraint is %s\n\n", carrierConstraint.c_str());
                }
                delete srcCarrier;
                srcCarrier = nullptr;
            }
        }
    } else {
        srcIsTopic = true;
    }

    // see if we can do business with the destination port
    bool destIsCompetent = false;
    bool destIsTopic = false;
    if (staticDest.getCarrier()!="topic") {
        if (!topical) {
            Carrier *destCarrier = nullptr;
            CARRIER_DEBUG("staticDest.getCarrier= %s  ", staticDest.getCarrier().c_str());
            if (staticDest.getCarrier()!="") {
                destCarrier = Carriers::chooseCarrier(staticDest.getCarrier().c_str());
            }
            if (destCarrier!=nullptr) {
                CARRIER_DEBUG("destCarrier is NOT null; its name is %s;  ", destCarrier->getName().c_str());
                std::string destBootstrap = destCarrier->getBootstrapCarrierName();
                if (destBootstrap!="") {
                    CARRIER_DEBUG(" it is competent(bootstrapname is %s), while its name is %s )\n\n\n\n", destBootstrap.c_str(), destCarrier->getName().c_str() );
                    destIsCompetent = true;
                } else {
                    //if the destCarrier is not competent, (that is it can't perform the starting yarp handshaking)
                    //set the carrier contraint equal to the carrier with which the posrt had been registered.
                    carrierConstraint = staticDest.getCarrier();
                    CARRIER_DEBUG(" it is NOT competent. its constraint is %s\n\n", carrierConstraint.c_str());
                }
                delete destCarrier;
                destCarrier = nullptr;
            }
        }
    } else {
        destIsTopic = true;
    }

    if (srcIsTopic||destIsTopic) {
        Bottle cmd, reply;
        NameSpace& ns = getNameSpace();

        bool ok = false;
        if (srcIsTopic) {
            if (mode==YARP_ENACT_CONNECT) {
                ok = ns.connectTopicToPort(staticSrc, staticDest, style);
            } else if (mode==YARP_ENACT_DISCONNECT) {
                ok = ns.disconnectTopicFromPort(staticSrc, staticDest, style);
            } else {
                fprintf(stderr, "Failure: cannot check subscriptions yet\n");
                return 1;
            }
        } else {
            if (mode==YARP_ENACT_CONNECT) {
                ok = ns.connectPortToTopic(staticSrc, staticDest, style);
            } else if (mode==YARP_ENACT_DISCONNECT) {
                ok = ns.disconnectPortFromTopic(staticSrc, staticDest, style);
            } else {
                fprintf(stderr, "Failure: cannot check subscriptions yet\n");
                return 1;
            }
        }
        if (!ok) {
            return 1;
        }
        if (!style.quiet) {
            if (style.verboseOnSuccess) {
                fprintf(stderr, "Success: connection to topic %s.\n", mode==YARP_ENACT_CONNECT ? "added" : "removed");
            }
        }
        return 0;
    }

    CARRIER_DEBUG("---------\n");
    CARRIER_DEBUG("dynamicSrc.getCarrier() = %s\n ", dynamicSrc.getCarrier().c_str());
    CARRIER_DEBUG("dynamicDest.getCarrier() = %s\n ", dynamicDest.getCarrier().c_str());
    CARRIER_DEBUG("staticSrc.getCarrier() = %s\n ", staticSrc.getCarrier().c_str());
    CARRIER_DEBUG("staticDest.getCarrier() = %s\n ", staticDest.getCarrier().c_str());
    CARRIER_DEBUG("carrierConstraint is %s\n ", carrierConstraint.c_str());
    CARRIER_DEBUG("---------\n");

    CARRIER_DEBUG("style.carrier (1) is %s\n ", style.carrier.c_str());


    if (dynamicSrc.getCarrier()!="") { //if in connect command the user specified the carrier of src port
        style.carrier = dynamicSrc.getCarrier();
        CARRIER_DEBUG("style.carrier is %s ==> in connect command the user specified the carrier of src port\n ", style.carrier.c_str());
    }

    if (dynamicDest.getCarrier()!="") { //if in connect command the user specified the carrier of dest port or the carrier of the connection
        style.carrier = dynamicDest.getCarrier();
         CARRIER_DEBUG("style.carrier is %s ==> in connect command the user specified the carrier of dest port or the carrier of the connection\n ", style.carrier.c_str());
    }

    CARRIER_DEBUG("at the end style style.carrier is %s\n ", style.carrier.c_str());

    //here we'll check if the style carrier and the contraint carrier are equal.
    //note that in both string may contain params of carrier, so we need to comapare only the name of carrier.
    if(style.carrier!="" && carrierConstraint!="") {
        //get only carrier name of style.
        std::string style_carrier_name = extractCarrierNameOnly(style.carrier);

        //get only carrier name of carrierConstraint.
        std::string carrier_constraint_name = extractCarrierNameOnly(carrierConstraint);

       if (style_carrier_name!=carrier_constraint_name) {
            fprintf(stderr, "Failure: conflict between %s and %s\n",
                    style_carrier_name.c_str(),
                    carrier_constraint_name.c_str());
            return 1;
        }
       CARRIER_DEBUG("style_carrier_name=%s and carrier_constraint_name=%s are equals!\n", style_carrier_name.c_str(), carrier_constraint_name.c_str());

    }
    //we are going to choose the carrier of this connection, and we collect parameters specified by user
    //in order to pass them to the carrier, so it can configure itself.
    if (carrierConstraint!="") {
        style.carrier = carrierConstraint;
        //if I'm here means that sorce or dest is not competent.
        //so we need to get parameters of carrier given in connect command.
        CARRIER_DEBUG("if I'm here means that sorce or dest is not competent\n");
        std::string c = dynamicSrc.getCarrier();
        if(extractCarrierNameOnly(c) == extractCarrierNameOnly(style.carrier))
            style.carrier+=collectParams(dynamicSrc);
        c = dynamicDest.getCarrier();
        if(extractCarrierNameOnly(c) == extractCarrierNameOnly(style.carrier))
            style.carrier+=collectParams(dynamicDest);
    }
    if (style.carrier=="") {
        style.carrier = staticDest.getCarrier();
        //if I'm here means that both src and dest are copentent and the user didn't specified a carrier in the connect command
        CARRIER_DEBUG("if I'm here means that both src and dest are copentent and the user didn't specified a carrier in the connect command\n");
        std::string c = dynamicSrc.getCarrier();
        if(extractCarrierNameOnly(c) == extractCarrierNameOnly(style.carrier))
            style.carrier+=collectParams(staticSrc);
    }

    if (style.carrier=="") {
        style.carrier = staticSrc.getCarrier();
        CARRIER_DEBUG("the chosen style carrier is static src\n ");
    }

    //now stylecarrier contains the carrier chosen for this connection

    CARRIER_DEBUG("style_carrier with params  =%s\n", style.carrier.c_str());

    bool connectionIsPush = false;
    bool connectionIsPull = false;
    Carrier *connectionCarrier = nullptr;
    if (style.carrier!="topic") {
        connectionCarrier = Carriers::chooseCarrier(style.carrier.c_str());
        if (connectionCarrier!=nullptr) {
            connectionIsPush = connectionCarrier->isPush();
            connectionIsPull = !connectionIsPush;
        }
    }

    int result = -1;
    if ((srcIsCompetent&&connectionIsPush)||topical) {
        // Classic case.
        Contact c = Contact::fromString(dest);
        if (connectionCarrier!=nullptr) delete connectionCarrier;
        return enactConnection(staticSrc, c, style, mode, false);
    }
    if (destIsCompetent&&connectionIsPull) {
        Contact c = Contact::fromString(src);
        if (connectionCarrier!=nullptr) delete connectionCarrier;
        return enactConnection(staticDest, c, style, mode, true);
    }

    if (connectionCarrier!=nullptr) {
        if (!connectionIsPull) {
            Contact c = Contact::fromString(dest);
            result = connectionCarrier->connect(staticSrc, c, style, mode, false);
        } else {
            Contact c = Contact::fromString(src);
            result = connectionCarrier->connect(staticDest, c, style, mode, true);
        }
    }
    if (connectionCarrier!=nullptr) {
        delete connectionCarrier;
        connectionCarrier = nullptr;
    }
    if (result!=-1) {
        if (!style.quiet) {
            if (result==0) {
                if (style.verboseOnSuccess) {
                    printf("Success: added connection using custom carrier method\n");
                }
            } else {
                printf("Failure: custom carrier method did not work\n");
            }
        }
        return result;
    }

    if (mode!=YARP_ENACT_DISCONNECT) {
        fprintf(stderr, "Failure: no way to make connection %s->%s\n", src.c_str(), dest.c_str());
    }

    return 1;
}

bool NetworkBase::connect(const std::string& src, const std::string& dest,
                          const std::string& carrier,
                          bool quiet) {
    ContactStyle style;
    style.quiet = quiet;
    if (carrier!="") {
        style.carrier = carrier;
    }
    return connect(src, dest, style);
}

bool NetworkBase::connect(const std::string& src,
                          const std::string& dest,
                          const ContactStyle& style) {
    int result = metaConnect(src, dest, style, YARP_ENACT_CONNECT);
    return result == 0;
}

bool NetworkBase::disconnect(const std::string& src,
                             const std::string& dest,
                             bool quiet) {
    ContactStyle style;
    style.quiet = quiet;
    return disconnect(src, dest, style);
}

bool NetworkBase::disconnect(const std::string& src,
                             const std::string& dest,
                             const ContactStyle& style) {
    int result = metaConnect(src, dest, style, YARP_ENACT_DISCONNECT);
    return result == 0;
}

bool NetworkBase::isConnected(const std::string& src,
                              const std::string& dest,
                              bool quiet) {
    ContactStyle style;
    style.quiet = quiet;
    return isConnected(src, dest, style);
}

bool NetworkBase::exists(const std::string& port, bool quiet) {
    ContactStyle style;
    style.quiet = quiet;
    return exists(port, style);
}

bool NetworkBase::exists(const std::string& port, const ContactStyle& style) {
    int result = Companion::exists(port.c_str(), style);
    if (result==0) {
        //Companion::poll(port, true);
        ContactStyle style2 = style;
        style2.admin = true;
        Bottle cmd("[ver]"), resp;
        bool ok = NetworkBase::write(Contact(port), cmd, resp, style2);
        if (!ok) result = 1;
        if (resp.get(0).toString()!="ver"&&resp.get(0).toString()!="dict") {
            // YARP nameserver responds with a version
            // ROS nameserver responds with a dictionary of error data
            // Treat everything else an unknown
            result = 1;
        }
    }
    return result == 0;
}


bool NetworkBase::sync(const std::string& port, bool quiet) {
    int result = Companion::wait(port.c_str(), quiet);
    if (result==0) {
        Companion::poll(port.c_str(), true);
    }
    return result == 0;
}

int NetworkBase::main(int argc, char *argv[]) {
    return Companion::main(argc, argv);
}

void NetworkBase::autoInitMinimum() {
    autoInitMinimum(YARP_CLOCK_DEFAULT);
}

void NetworkBase::autoInitMinimum(yarp::os::yarpClockType clockType, yarp::os::Clock *custom) {
    YARP_UNUSED(custom);
    if (!(__yarp_auto_init_active||__yarp_is_initialized)) {
        __yarp_auto_init_active = true;
        initMinimum(clockType);
    }
}


void NetworkBase::initMinimum() {
    initMinimum(YARP_CLOCK_DEFAULT);
}

void NetworkBase::initMinimum(yarp::os::yarpClockType clockType, yarp::os::Clock *custom) {
    YARP_UNUSED(custom);
    if (__yarp_is_initialized==0) {
        // Broken pipes need to be dealt with through other means
        yarp::os::impl::signal(SIGPIPE, SIG_IGN);

#ifdef YARP_HAS_ACE
        ACE::init();
#endif
        ThreadImpl::init();
        BottleImpl::getNull();
        Bottle::getNullBottle();
        std::string quiet = getEnvironment("YARP_QUIET");
        Bottle b2(quiet.c_str());
        if (b2.get(0).asInt32()>0) {
            Logger::get().setVerbosity(-b2.get(0).asInt32());
        } else {
            std::string verbose = getEnvironment("YARP_VERBOSE");
            Bottle b(verbose.c_str());
            if (b.get(0).asInt32()>0) {
                YARP_INFO(Logger::get(),
                          "YARP_VERBOSE environment variable is set");
                Logger::get().setVerbosity(b.get(0).asInt32());
            }
        }
        std::string stack = getEnvironment("YARP_STACK_SIZE");
        if (stack!="") {
            int sz = atoi(stack.c_str());
            Thread::setDefaultStackSize(sz);
            YARP_SPRINTF1(Logger::get(), info,
                          "YARP_STACK_SIZE set to %d", sz);
        }

        // make sure system is actually able to do things fast
        Time::turboBoost();

        // prepare carriers
        Carriers::getInstance();
        __yarp_is_initialized++;
        if(yarp::os::Time::getClockType() == YARP_CLOCK_UNINITIALIZED)
            Network::yarpClockInit(clockType, nullptr);
    }
    else
        __yarp_is_initialized++;
}

void NetworkBase::finiMinimum() {
    if (__yarp_is_initialized==1) {
        Time::useSystemClock();
        removeNameSpace();
        Bottle::fini();
        BottleImpl::fini();
        ThreadImpl::fini();
        yarp::os::impl::removeClock();
#ifdef YARP_HAS_ACE
        ACE::fini();
#endif
    }
    if (__yarp_is_initialized>0) __yarp_is_initialized--;
}

void yarp::os::Network::yarpClockInit(yarp::os::yarpClockType clockType, Clock *custom)
{
    std::string clock="";
    if(clockType == YARP_CLOCK_DEFAULT)
    {
        clock = yarp::os::Network::getEnvironment("YARP_CLOCK");
        if(!clock.empty())
            clockType = YARP_CLOCK_NETWORK;
        else
            clockType = YARP_CLOCK_SYSTEM;
    }

    switch(clockType)
    {
        case YARP_CLOCK_SYSTEM:
            YARP_DEBUG(Logger::get(), "Using SYSTEM clock");
            yarp::os::Time::useSystemClock();
        break;

        case YARP_CLOCK_NETWORK:
            YARP_DEBUG(Logger::get(), "Using NETWORK clock");
            // check of valid parameter is done inside the call, throws YARP_FAIL in case of error
            yarp::os::Time::useNetworkClock(clock);
        break;

        case YARP_CLOCK_CUSTOM:
        {
            YARP_DEBUG(Logger::get(), "Using CUSTOM clock");
            // check of valid parameter is done inside the call, throws YARP_FAIL in case of error
            yarp::os::Time::useCustomClock(custom);
        }
        break;

        default:
            YARP_FAIL(Logger::get(), "yarpClockInit called with unknown clock type. Quitting");
        break;
    }
    return;
}

Contact NetworkBase::queryName(const std::string& name) {
    YARP_SPRINTF1(Logger::get(), debug, "query name %s", name.c_str());
    if (getNameServerName()==name) {
        YARP_SPRINTF1(Logger::get(), debug, "query recognized as name server: %s", name.c_str());
        return getNameServerContact();
    }
    Contact c = c.fromString(name);
    if (c.isValid()&&c.getPort()>0) {
        return c;
    }
    return getNameSpace().queryName(name);
}


Contact NetworkBase::registerName(const std::string& name) {
    YARP_SPRINTF1(Logger::get(), debug, "register name %s", name.c_str());
    return getNameSpace().registerName(name);
}


Contact NetworkBase::registerContact(const Contact& contact) {
    YARP_SPRINTF1(Logger::get(), debug, "register contact %s",
                  contact.toString().c_str());
    return getNameSpace().registerContact(contact);
}

Contact NetworkBase::unregisterName(const std::string& name) {
    return getNameSpace().unregisterName(name);
}


Contact NetworkBase::unregisterContact(const Contact& contact) {
    return getNameSpace().unregisterContact(contact);
}


bool NetworkBase::setProperty(const char *name,
                              const char *key,
                              const Value& value) {
    return getNameSpace().setProperty(name, key, value);
}


Value *NetworkBase::getProperty(const char *name, const char *key) {
    return getNameSpace().getProperty(name, key);
}


bool NetworkBase::setLocalMode(bool flag) {
    return getNameSpace().setLocalMode(flag);
}

bool NetworkBase::getLocalMode() {
    NameSpace& ns = getNameSpace();
    return ns.localOnly();
}

void NetworkBase::assertion(bool shouldBeTrue) {
    // could replace with ACE assertions, except should not
    // evaporate in release mode
    yAssert(shouldBeTrue);
}


std::string NetworkBase::readString(bool *eof) {
    return std::string(Companion::readString(eof).c_str());
}

bool NetworkBase::setConnectionQos(const std::string& src, const std::string& dest,
                                   const QosStyle& style, bool quiet) {
    return setConnectionQos(src, dest, style, style, quiet);
}

bool NetworkBase::setConnectionQos(const std::string& src, const std::string& dest,
                                   const QosStyle& srcStyle, const QosStyle &destStyle,
                                   bool quiet) {

    //e.g.,  prop set /portname (sched ((priority 30) (policy 1))) (qos ((tos 0)))
    yarp::os::Bottle cmd, reply;

    // ignore if everything left as default
    if (srcStyle.getPacketPriorityAsTOS()!=-1 || srcStyle.getThreadPolicy() !=-1) {
        // set the source Qos
        cmd.addString("prop");
        cmd.addString("set");
        cmd.addString(dest.c_str());
        Bottle& sched = cmd.addList();
        sched.addString("sched");
        Property& sched_prop = sched.addDict();
        sched_prop.put("priority", srcStyle.getThreadPriority());
        sched_prop.put("policy", srcStyle.getThreadPolicy());
        Bottle& qos = cmd.addList();
        qos.addString("qos");
        Property& qos_prop = qos.addDict();
        qos_prop.put("tos", srcStyle.getPacketPriorityAsTOS());
        Contact srcCon = Contact::fromString(src);
        bool ret = write(srcCon, cmd, reply, true, true, 2.0);
        if (!ret) {
            if (!quiet)
                 fprintf(stderr, "Cannot write to '%s'\n", src.c_str());
            return false;
        }
        if (reply.get(0).asString() != "ok") {
            if (!quiet)
                 fprintf(stderr, "Cannot set qos properties of '%s'. (%s)\n",
                                 src.c_str(), reply.toString().c_str());
            return false;
        }
    }

    // ignore if everything left as default
    if (destStyle.getPacketPriorityAsTOS()!=-1 || destStyle.getThreadPolicy() !=-1) {
        // set the destination Qos
        cmd.clear();
        reply.clear();
        cmd.addString("prop");
        cmd.addString("set");
        cmd.addString(src.c_str());
        Bottle& sched2 = cmd.addList();
        sched2.addString("sched");
        Property& sched_prop2 = sched2.addDict();
        sched_prop2.put("priority", destStyle.getThreadPriority());
        sched_prop2.put("policy", destStyle.getThreadPolicy());
        Bottle& qos2 = cmd.addList();
        qos2.addString("qos");
        Property& qos_prop2 = qos2.addDict();
        qos_prop2.put("tos", destStyle.getPacketPriorityAsTOS());
        Contact destCon = Contact::fromString(dest);
        bool ret = write(destCon, cmd, reply, true, true, 2.0);
        if (!ret) {
            if (!quiet)
                 fprintf(stderr, "Cannot write to '%s'\n", dest.c_str());
            return false;
        }
        if (reply.get(0).asString() != "ok") {
            if (!quiet)
                 fprintf(stderr, "Cannot set qos properties of '%s'. (%s)\n",
                                 dest.c_str(), reply.toString().c_str());
            return false;
        }
    }
    return true;
}

static bool getPortQos(const std::string& port, const std::string& unit,
                             QosStyle& style, bool quiet) {
    // request: "prop get /portname"
    // reply  : "(sched ((priority 30) (policy 1))) (qos ((priority HIGH)))"
    yarp::os::Bottle cmd, reply;

    // set the source Qos
    cmd.addString("prop");
    cmd.addString("get");
    cmd.addString(unit.c_str());
    Contact portCon = Contact::fromString(port);
    bool ret = NetworkBase::write(portCon, cmd, reply, true, true, 2.0);
    if (!ret) {
        if (!quiet)
             fprintf(stderr, "Cannot write to '%s'\n", port.c_str());
        return false;
    }
    if (reply.size() == 0 || reply.get(0).asString() == "fail") {
        if (!quiet)
             fprintf(stderr, "Cannot get qos properties of '%s'. (%s)\n",
                             port.c_str(), reply.toString().c_str());
        return false;
    }

    Bottle& sched = reply.findGroup("sched");
    Bottle* sched_prop = sched.find("sched").asList();
    style.setThreadPriority(sched_prop->find("priority").asInt32());
    style.setThreadPolicy(sched_prop->find("policy").asInt32());
    Bottle& qos = reply.findGroup("qos");
    Bottle* qos_prop = qos.find("qos").asList();
    style.setPacketPrioritybyTOS(qos_prop->find("tos").asInt32());

    return true;
}

bool NetworkBase::getConnectionQos(const std::string& src, const std::string& dest,
                                   QosStyle& srcStyle, QosStyle& destStyle, bool quiet) {
    if (!getPortQos(src, dest, srcStyle, quiet))
        return false;
    if (!getPortQos(dest, src, destStyle, quiet))
        return false;
    return true;
}

bool NetworkBase::isValidPortName(const std::string& portName)
{
    if (portName.empty())
    {
        return false;
    }

    if (portName == "...")
    {
       return true;
    }

    if (portName.at(0) != '/')
    {
        return false;
    }

    if (portName.at(portName.size()-1) == '/')
    {
        return false;
    }

    if (portName.find(" ") != std::string::npos)
    {
        return false;
    }

    return true;
}


bool NetworkBase::write(const Contact& contact,
                       PortWriter& cmd,
                       PortReader& reply,
                       bool admin,
                       bool quiet,
                       double timeout) {
    ContactStyle style;
    style.admin = admin;
    style.quiet = quiet;
    style.timeout = timeout;
    style.carrier = contact.getCarrier();
    return write(contact, cmd, reply, style);
}

bool NetworkBase::write(const Contact& contact,
                        PortWriter& cmd,
                        PortReader& reply,
                        const ContactStyle& style) {
    if (!getNameSpace().serverAllocatesPortNumbers()) {
        // switch to more up-to-date method

        Port port;
        port.setAdminMode(style.admin);
        port.openFake("network_write");
        Contact ec = contact;
        if (style.carrier!="") {
            ec.setCarrier(style.carrier);
        }
        if (!port.addOutput(ec)) {
            if (!style.quiet) {
                fprintf(stderr, "Cannot make connection to '%s'\n",
                                ec.toString().c_str());
            }
            return false;
        }

        bool ok = port.write(cmd, reply);
        return ok;
    }

    const char *connectionName = "admin";
    std::string name = contact.getName();
    const char *targetName = name.c_str();  // use carefully!
    Contact address = contact;
    if (!address.isValid()) {
        address = getNameSpace().queryName(targetName);
    }
    if (!address.isValid()) {
        if (!style.quiet) {
            YARP_SPRINTF1(Logger::get(), error,
                          "cannot find port %s",
                          targetName);
        }
        return false;
    }

    if (style.timeout>0) {
        address.setTimeout((float)style.timeout);
    }
    OutputProtocol *out = Carriers::connect(address);
    if (out==nullptr) {
        if (!style.quiet) {
            YARP_SPRINTF1(Logger::get(), error,
                          "Cannot connect to port %s",
                          targetName);
        }
        return false;
    }
    if (style.timeout>0) {
        out->setTimeout(style.timeout);
    }

    Route r(connectionName, targetName,
            (style.carrier!="")?style.carrier.c_str():"text_ack");
    out->open(r);

    PortCommand pc(0, style.admin?"a":"d");
    BufferedConnectionWriter bw(out->getConnection().isTextMode(),
                                out->getConnection().isBareMode());
    bool ok = true;
    if (out->getConnection().canEscape()) {
        ok = pc.write(bw);
    }
    if (!ok) {
        if (!style.quiet) {
            YARP_ERROR(Logger::get(), "could not write to connection");
        }
        if (out!=nullptr) delete out;
        return false;
    }
    ok = cmd.write(bw);
    if (!ok) {
        if (!style.quiet) {
            YARP_ERROR(Logger::get(), "could not write to connection");
        }
        if (out!=nullptr) delete out;
        return false;
    }
    if (style.expectReply) {
        bw.setReplyHandler(reply);
    }
    out->write(bw);
    if (out!=nullptr) {
        delete out;
        out = nullptr;
    }
    return true;
}

bool NetworkBase::write(const std::string& port_name,
                              PortWriter& cmd,
                              PortReader& reply) {
    return write(Contact(port_name), cmd, reply);
}

bool NetworkBase::isConnected(const std::string& src, const std::string& dest,
                              const ContactStyle& style) {
    int result = metaConnect(src, dest, style, YARP_ENACT_EXISTS);
    if (result!=0) {
        if (!style.quiet) {
            printf("No connection from %s to %s found\n",
                   src.c_str(), dest.c_str());
        }
    }
    return result == 0;
}


std::string NetworkBase::getNameServerName() {
    NameConfig nc;
    std::string name = nc.getNamespace(false);
    return name.c_str();
}


Contact NetworkBase::getNameServerContact() {
    return getNameSpace().getNameServerContact();
}



bool NetworkBase::setNameServerName(const std::string& name) {
    NameConfig nc;
    std::string fname = nc.getConfigFileName(YARP_CONFIG_NAMESPACE_FILENAME);
    nc.writeConfig(fname, name + "\n");
    nc.getNamespace(true);
    getNameSpace().activate(true);
    return true;
}


bool NetworkBase::checkNetwork() {
    return getNameSpace().checkNetwork();
}


bool NetworkBase::checkNetwork(double timeout) {
    return getNameSpace().checkNetwork(timeout);
}


bool NetworkBase::initialized() {
    return __yarp_is_initialized>0;
}


void NetworkBase::setVerbosity(int verbosity) {
    Logger::get().setVerbosity(verbosity);
}

void NetworkBase::queryBypass(NameStore *store) {
    getNameSpace().queryBypass(store);
}

NameStore *NetworkBase::getQueryBypass() {
    return getNameSpace().getQueryBypass();
}



std::string NetworkBase::getEnvironment(const char *key,
                                        bool *found) {
    const char *result = yarp::os::impl::getenv(key);
    if (found != nullptr) {
        *found = (result!=nullptr);
    }
    if (result == nullptr) {
        return "";
    }
    return result;
}

void NetworkBase::setEnvironment(const std::string& key, const std::string& val) {
    yarp::os::impl::setenv(key.c_str(), val.c_str(), 1);
}

void NetworkBase::unsetEnvironment(const std::string& key) {
    yarp::os::impl::unsetenv(key.c_str());
}

std::string NetworkBase::getDirectorySeparator() {
#if defined(_WIN32)
    // note this may be wrong under cygwin
    // should be ok for mingw
    return "\\";
#else
    return "/";
#endif
}

std::string NetworkBase::getPathSeparator() {
#if defined(_WIN32)
    // note this may be wrong under cygwin
    // should be ok for mingw
    return ";";
#else
    return ":";
#endif
}

void NetworkBase::lock() {
    ThreadImpl::init();
    ThreadImpl::threadMutex->wait();
}

void NetworkBase::unlock() {
    ThreadImpl::init();
    ThreadImpl::threadMutex->post();
}


class ForwardingCarrier : public Carrier {
public:
    SharedLibraryClassFactory<Carrier> *factory;
    SharedLibraryClass<Carrier> car;
    Carrier *owner;

    ForwardingCarrier() {
        owner = nullptr;
        factory = nullptr;
    }

    ForwardingCarrier(SharedLibraryClassFactory<Carrier> *factory,
                      Carrier *owner) :
        factory(factory),
        owner(owner)
    {
        factory->addRef();
        car.open(*factory);
    }

    virtual ~ForwardingCarrier() {
        car.close();
        if (!factory) return;
        factory->removeRef();
        if (factory->getReferenceCount()<=0) {
            delete factory;
        }
        factory = nullptr;
    }


    virtual Carrier& getContent() {
        return car.getContent();
    }

    virtual Carrier *create() override {
        return owner->create();
    }


    // Forward yarp::os::Connection methods

    bool isValid() override {
        return car.isValid();
    }

    virtual bool isTextMode() override {
        return getContent().isTextMode();
    }

    virtual bool isBareMode() override {
        return getContent().isBareMode();
    }

    virtual bool canEscape() override {
        return getContent().canEscape();
    }

    virtual void handleEnvelope(const std::string& envelope) override {
        getContent().handleEnvelope(envelope);
    }

    virtual bool requireAck() override {
        return getContent().requireAck();
    }

    virtual bool supportReply() override {
        return getContent().supportReply();
    }

    virtual bool isLocal() override {
        return getContent().isLocal();
    }

    virtual bool isPush() override {
        return getContent().isPush();
    }

    virtual bool isConnectionless() override {
        return getContent().isConnectionless();
    }

    virtual bool isBroadcast() override {
        return getContent().isBroadcast();
    }

    virtual bool isActive() override {
        return getContent().isActive();
    }

    virtual bool modifiesIncomingData() override {
        return getContent().modifiesIncomingData();
    }

    virtual ConnectionReader& modifyIncomingData(ConnectionReader& reader) override {
        return getContent().modifyIncomingData(reader);
    }

    virtual bool acceptIncomingData(ConnectionReader& reader) override {
        return getContent().acceptIncomingData(reader);
    }

    virtual bool modifiesOutgoingData() override {
        return getContent().modifiesOutgoingData();
    }

    virtual PortWriter& modifyOutgoingData(PortWriter& writer) override {
        return getContent().modifyOutgoingData(writer);
    }

    virtual bool acceptOutgoingData(PortWriter& writer) override {
        return getContent().acceptOutgoingData(writer);
    }

    virtual bool modifiesReply() override {
        return getContent().modifiesReply();
    }

    virtual PortReader& modifyReply(PortReader& reader) override {
        return getContent().modifyReply(reader);
    }

    virtual void setCarrierParams(const Property& params) override {
        getContent().setCarrierParams(params);
    }

    virtual void getCarrierParams(Property& params) override {
        getContent().getCarrierParams(params);
    }

    virtual void getHeader(const yarp::os::Bytes& header) override {
        getContent().getHeader(header);
    }

    virtual void prepareDisconnect() override {
        getContent().prepareDisconnect();
    }

    virtual std::string getName() override {
        return getContent().getName();
    }


    // Forward yarp::os::Carrier methods

    virtual bool checkHeader(const yarp::os::Bytes& header) override {
        return getContent().checkHeader(header);
    }

    virtual void setParameters(const yarp::os::Bytes& header) override {
        getContent().setParameters(header);
    }

    virtual bool canAccept() override {
        return getContent().canAccept();
    }

    virtual bool canOffer() override {
        return getContent().canOffer();
    }

    virtual bool prepareSend(ConnectionState& proto) override {
        return getContent().prepareSend(proto);
    }

    virtual bool sendHeader(ConnectionState& proto) override {
        return getContent().sendHeader(proto);
    }

    virtual bool expectReplyToHeader(ConnectionState& proto) override {
        return getContent().expectReplyToHeader(proto);
    }

    virtual bool write(ConnectionState& proto, SizedWriter& writer) override {
        return getContent().write(proto, writer);
    }

    virtual bool reply(ConnectionState& proto, SizedWriter& writer) override {
        return getContent().reply(proto, writer);
    }

    virtual bool expectExtraHeader(ConnectionState& proto) override {
        return getContent().expectExtraHeader(proto);
    }

    virtual bool respondToHeader(ConnectionState& proto) override {
        return getContent().respondToHeader(proto);
    }

    virtual bool expectIndex(ConnectionState& proto) override {
        return getContent().expectIndex(proto);
    }

    virtual bool expectSenderSpecifier(ConnectionState& proto) override {
        return getContent().expectSenderSpecifier(proto);
    }

    virtual bool sendAck(ConnectionState& proto) override {
        return getContent().sendAck(proto);
    }

    virtual bool expectAck(ConnectionState& proto) override {
        return getContent().expectAck(proto);
    }

    virtual std::string toString() override {
        return getContent().toString();
    }

    virtual void close() override {
        getContent().close();
    }

    virtual std::string getBootstrapCarrierName() override {
        return getContent().getBootstrapCarrierName();
    }

    virtual int connect(const yarp::os::Contact& src,
                        const yarp::os::Contact& dest,
                        const yarp::os::ContactStyle& style,
                        int mode,
                        bool reversed) override {
        return getContent().connect(src, dest, style, mode, reversed);
    }

    virtual bool configure(ConnectionState& proto) override {
        return getContent().configure(proto);
    }
    virtual bool configureFromProperty(yarp::os::Property& options) override {
        return getContent().configureFromProperty(options);
    }

    virtual yarp::os::Face* createFace(void) override {
        return getContent().createFace();
    }
};


class StubCarrier : public ForwardingCarrier {
private:
    YarpPluginSettings settings;
    YarpPlugin<Carrier> plugin;
public:
    StubCarrier(const char *dll_name, const char *fn_name) {
        settings.setLibraryMethodName(dll_name, fn_name);
        init();
    }

    StubCarrier(const char *name) {
        settings.setPluginName(name);
        init();
    }

    void init() {
        YarpPluginSelector selector;
        selector.scan();
        settings.setSelector(selector);
        if (plugin.open(settings)) {
            car.open(*plugin.getFactory());
            settings.setLibraryMethodName(plugin.getFactory()->getName(),
                                          settings.getMethodName());
        }
    }

    Carrier& getContent() override {
        return car.getContent();
    }

    virtual Carrier *create() override {
        ForwardingCarrier *ncar = new ForwardingCarrier(plugin.getFactory(), this);
        if (ncar==nullptr) {
            return nullptr;
        }
        if (!ncar->isValid()) {
            delete ncar;
            ncar = nullptr;
            return nullptr;
        }
        return ncar;
    }

    std::string getDllName() const {
       return settings.getLibraryName();
    }

    std::string getFnName() const {
        return settings.getMethodName();
    }
};


bool NetworkBase::registerCarrier(const char *name, const char *dll) {
    StubCarrier *factory = nullptr;
    if (dll==nullptr) {
        factory = new StubCarrier(name);
        if (!factory) return false;
    } else {
        factory = new StubCarrier(dll, name);
    }
    if (factory==nullptr) {
        YARP_ERROR(Logger::get(), "Failed to register carrier");
        return false;
    }
    if (!factory->isValid()) {
        if (dll!=nullptr) {
            YARP_SPRINTF2(Logger::get(), error, "Failed to find library %s with carrier %s", dll, name);
        } else {
            YARP_SPRINTF1(Logger::get(), error, "Failed to find library support for carrier %s", name);
        }
        delete factory;
        factory = nullptr;
        return false;
    }
    Carriers::addCarrierPrototype(factory);
    return true;
}


bool NetworkBase::localNetworkAllocation() {
    bool globalAlloc = getNameSpace().serverAllocatesPortNumbers();
    return !globalAlloc;
}


Contact NetworkBase::detectNameServer(bool useDetectedServer,
                                      bool& scanNeeded,
                                      bool& serverUsed) {
    return getNameSpace().detectNameServer(useDetectedServer,
                                           scanNeeded,
                                           serverUsed);
}

bool NetworkBase::setNameServerContact(Contact &nameServerContact)
{
    NameConfig nameConfig;
    if (nameServerContact.getName() != "")
        setNameServerName(nameServerContact.getName());
    nameConfig.fromFile();
    nameConfig.setAddress(nameServerContact);
    bool result = nameConfig.toFile();
    getNameSpace().activate(true);
    return result;
}


bool NetworkBase::writeToNameServer(PortWriter& cmd,
                                    PortReader& reply,
                                    const ContactStyle& style) {
    NameStore *store = getNameSpace().getQueryBypass();
    if (store) {
        Contact contact;
        return store->process(cmd, reply, contact);
    }
    return getNameSpace().writeToNameServer(cmd, reply, style);
}


std::string NetworkBase::getConfigFile(const char *fname) {
    return NameConfig::expandFilename(fname).c_str();
}


int NetworkBase::getDefaultPortRange() {
    std::string range = NetworkBase::getEnvironment("YARP_PORT_RANGE");
    if (range!="") {
        int irange = NetType::toInt(range.c_str());
        if (irange != 0) return irange;
    }
    return 10000;
}
