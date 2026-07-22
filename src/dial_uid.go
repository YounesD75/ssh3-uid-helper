// dial_uid.go — Go-side of the privilege-separation helper.
//
// DialUID connects to the C++ helper daemon, requests a TCP socket
// created under the given uid/gid, receives the file descriptor via
// SCM_RIGHTS, and wraps it as a net.Conn ready for use by SSH3.
//
// Drop this file into the ssh3 module (e.g. alongside ssh3-server.go)
// and replace the net.DialTCP call in handleTCPForwardingChannel with:
//
//   conn, err := dialUID(uint32(user.Uid), uint32(user.Gid), channel.RemoteAddr)
//
// The helper daemon (helper_daemon_v2) must be running before the server starts.

package cmd

import (
	"fmt"
	"net"
	"os"

	"golang.org/x/sys/unix"
)

const helperSockPath = "/tmp/ssh3-helper.sock"

// dialUID asks the helper daemon to create a TCP socket under uid/gid,
// connects it to addr, and returns the connection.
func dialUID(uid, gid uint32, addr *net.TCPAddr) (net.Conn, error) {
	// 1. Connect to the helper daemon over the UNIX socket
	daemonConn, err := net.Dial("unix", helperSockPath)
	if err != nil {
		return nil, fmt.Errorf("dialUID: cannot reach helper daemon at %s: %w",
			helperSockPath, err)
	}
	defer daemonConn.Close()

	// 2. Send the request: "uid=1000 gid=1000 host=127.0.0.1 port=8000\n"
	req := fmt.Sprintf("uid=%d gid=%d host=%s port=%d\n",
		uid, gid, addr.IP.String(), addr.Port)
	if _, err := fmt.Fprint(daemonConn, req); err != nil {
		return nil, fmt.Errorf("dialUID: write request: %w", err)
	}

	// 3. Receive the fd via SCM_RIGHTS
	//    We need the raw file descriptor of the UNIX connection to call Recvmsg.
	unixConn, ok := daemonConn.(*net.UnixConn)
	if !ok {
		return nil, fmt.Errorf("dialUID: expected *net.UnixConn")
	}

	rawConn, err := unixConn.SyscallConn()
	if err != nil {
		return nil, fmt.Errorf("dialUID: SyscallConn: %w", err)
	}

	// Buffers for Recvmsg
	buf := make([]byte, 1)            // dummy byte (the C sender sends '!')
	oob := make([]byte, unix.CmsgSpace(4)) // space for one int (the fd)

	var (
		oobn    int
		recvErr error
	)
	err = rawConn.Read(func(fd uintptr) bool {
		_, oobn, _, _, recvErr = unix.Recvmsg(int(fd), buf, oob, 0)
		return true // always done after one call
	})
	if err != nil {
		return nil, fmt.Errorf("dialUID: rawConn.Read: %w", err)
	}
	if recvErr != nil {
		return nil, fmt.Errorf("dialUID: Recvmsg: %w", recvErr)
	}

	// Parse the ancillary data to extract the fd
	scms, err := unix.ParseSocketControlMessage(oob[:oobn])
	if err != nil {
		return nil, fmt.Errorf("dialUID: ParseSocketControlMessage: %w", err)
	}
	if len(scms) == 0 {
		return nil, fmt.Errorf("dialUID: no control message received (daemon rejected request?)")
	}
	fds, err := unix.ParseUnixRights(&scms[0])
	if err != nil {
		return nil, fmt.Errorf("dialUID: ParseUnixRights: %w", err)
	}
	if len(fds) == 0 {
		return nil, fmt.Errorf("dialUID: no fd in control message")
	}
	receivedFd := fds[0]

	// 4. Wrap the raw fd as a net.Conn
	//    os.NewFile gives us an *os.File; net.FileConn wraps it as net.Conn.
	//    net.FileConn dups the fd internally, so we close our copy afterwards.
	file := os.NewFile(uintptr(receivedFd), "uid-socket")
	defer file.Close()

	conn, err := net.FileConn(file)
	if err != nil {
		return nil, fmt.Errorf("dialUID: net.FileConn: %w", err)
	}

	return conn, nil
}
