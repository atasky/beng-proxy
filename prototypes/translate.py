#!/usr/bin/python
#
# Prototype for the beng-proxy translation server.
#
# Author: Max Kellermann <mk@cm4all.com>

import re
from twisted.internet import reactor
from twisted.internet.protocol import Protocol, Factory
from beng_proxy.translation import *

cgi_re = re.compile('\.(?:sh|rb|py|pl|cgi|php\d?)$')

class Translation(Protocol):
    def connectionMade(self):
        print "Connected from", self.transport.client
        self._request = None
        self._packet = None

    def _handle_widget_lookup(self, request):
        # checks on the name should be here.
        path = "/etc/cm4all/beng/widgets/%s" % request.widget_type
        try:
            f = open(path)
        except IOError:
            response = Response()
            response.status(404)
            self.transport.write(response.finish())
            return

        response = Response()
        for line in f.readlines():
            line = line.strip()
            if line == '' or line[0] == '#':
                continue
            m = re.match(r'^server\s+"(\S+)"$', line)
            if m:
                uri = m.group(1)
                response.proxy(uri)
                continue
            m = re.match(r'^cgi\s+"(\S+)"$', line)
            if m:
                response.packet(TRANSLATE_CGI, m.group(1))
                continue
            m = re.match(r'^fastcgi\s+"(\S+)"$', line)
            if m:
                response.packet(TRANSLATE_FASTCGI, m.group(1))
                continue
            m = re.match(r'^ajp\s+"(\S+)"\s+"(\S+)"$', line)
            if m:
                host, uri = m.group(1), m.group(2)
                response.ajp(host, uri)
                continue
            m = re.match(r'^path\s+"(\S+)"$', line)
            if m:
                response.packet(TRANSLATE_PATH, m.group(1))
                continue
            m = re.match(r'^script_name\s+"(\S+)"$', line)
            if m:
                response.packet(TRANSLATE_SCRIPT_NAME, m.group(1))
                continue
            m = re.match(r'^document_root\s+"(\S+)"$', line)
            if m:
                response.packet(TRANSLATE_DOCUMENT_ROOT, m.group(1))
                continue
            m = re.match(r'^action\s+"(\S+)"$', line)
            if m:
                response.packet(TRANSLATE_ACTION, m.group(1))
                continue
            m = re.match(r'^interpreter\s+"(\S+)"$', line)
            if m:
                response.packet(TRANSLATE_INTERPRETER, m.group(1))
                continue
            if line == 'process':
                response.packet(TRANSLATE_PROCESS)
            elif line == 'container':
                response.packet(TRANSLATE_CONTAINER)
            elif line == 'stateful':
                response.packet(TRANSLATE_STATEFUL)
            else:
                print "Syntax error in %s: %s" % (path, line)
                response.status(500)
                break
        self.transport.write(response.finish())

    def _handle_request(self, request):
        if request.session is not None: print "- session =", request.session
        if request.param is not None: print "- param =", request.param

        if request.param is not None:
            # log in or log out; "real" authentification is missing
            # here.  We're logging out if request.param is an empty
            # string.
            user = session = request.param
        elif request.session is not None:
            # user is already authenticated
            user = request.session
            session = None
        else:
            # 
            user = session = None

        if request.widget_type is not None:
            return self._handle_widget_lookup(request)

        if request.uri[:19] == '/cm4all-beng-proxy/':
            from sys import argv
            if len(argv) >= 3:
                path = argv[2]
            else:
                from os.path import abspath, dirname, join
                path = join(dirname(dirname(abspath(argv[0]))), 'js/')
            path += request.uri[19:]
        else:
            path = '/var/www' + request.uri

        cgi = cgi_re.search(path, 1)

        response = Response()
        response.packet(TRANSLATE_DOCUMENT_ROOT, "/var/www")
        if cgi:
            response.packet(TRANSLATE_CGI, path)
        else:
            response.packet(TRANSLATE_PATH, path)
        if user is not None:
            response.packet(TRANSLATE_USER, user)
        if session is not None:
            response.packet(TRANSLATE_SESSION, session)
        #response.packet(TRANSLATE_FILTER)
        # .... PROXY 'http://cfatest01.intern.cm-ag/filter.py'
        if not cgi and path[-5:] == '.html':
            response.packet(TRANSLATE_CONTENT_TYPE, 'text/html; charset=utf-8')
            response.packet(TRANSLATE_PROCESS)
            response.packet(TRANSLATE_CONTAINER)
        self.transport.write(response.finish())

    def _handle_packet(self, packet):
        if packet.command == TRANSLATE_BEGIN:
            self._request = Request()
        elif self._request is not None:
            if self._request.packetReceived(packet):
                self._handle_request(self._request)
                self._request = None
        else:
            print "Invalid command without request:", packet.command

    def dataReceived(self, data):
        while len(data) > 0:
            if self._packet is None:
                self._packet = PacketReader()
            data = self._packet.consume(data)
            assert len(data) == 0 or self._packet.complete
            if self._packet.complete:
                self._handle_packet(self._packet)
                self._packet = None

    def connectionLost(self, reason):
        print "Disconnected from", self.transport.client

factory = Factory()
factory.protocol = Translation

if __name__ == '__main__':
    from sys import argv
    if len(argv) >= 2:
        path = argv[1]
    else:
        path = '/tmp/beng-proxy-translate'
    reactor.listenUNIX(path, factory)
    reactor.run()
