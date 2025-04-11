'use strict';

import { Socket } from 'net';
import { EventEmitter } from 'events';
import * as vscode from 'vscode';

// Debug configuration
const DEBUG_ENABLED = true;         // Master switch for all debugging
const DEBUG_MESSAGES = true;        // Enable logging of message content
const DEBUG_CONNECTIONS = true;     // Enable detailed connection logging

/**
 * Message types for communication with the AMXX server
 */
export enum MessageType {
    Diagnostics = 0,
    RequestFile,
    File,
    StartDebugging,
    StopDebugging,
    Pause,
    Continue,
    RequestCallStack,
    CallStack,
    ClearBreakpoints,
    SetBreakpoint,
    HasStopped,
    HasContinued,
    StepOver,
    StepIn,
    StepOut,
    RequestSetVariable,
    SetVariable,
    RequestVariables,
    Variables,
    RequestEvaluate,
    Evaluate,
    Disconnect,
    TotalMessages
}

/**
 * Mapping of message types to human-readable names for debugging
 */
const MessageTypeNames = {
    [MessageType.Diagnostics]: "Diagnostics",
    [MessageType.RequestFile]: "RequestFile",
    [MessageType.File]: "File",
    [MessageType.StartDebugging]: "StartDebugging",
    [MessageType.StopDebugging]: "StopDebugging",
    [MessageType.Pause]: "Pause",
    [MessageType.Continue]: "Continue",
    [MessageType.RequestCallStack]: "RequestCallStack", 
    [MessageType.CallStack]: "CallStack",
    [MessageType.ClearBreakpoints]: "ClearBreakpoints",
    [MessageType.SetBreakpoint]: "SetBreakpoint",
    [MessageType.HasStopped]: "HasStopped",
    [MessageType.HasContinued]: "HasContinued",
    [MessageType.StepOver]: "StepOver",
    [MessageType.StepIn]: "StepIn",
    [MessageType.StepOut]: "StepOut",
    [MessageType.RequestSetVariable]: "RequestSetVariable",
    [MessageType.SetVariable]: "SetVariable",
    [MessageType.RequestVariables]: "RequestVariables",
    [MessageType.Variables]: "Variables",
    [MessageType.RequestEvaluate]: "RequestEvaluate",
    [MessageType.Evaluate]: "Evaluate",
    [MessageType.Disconnect]: "Disconnect"
};

/**
 * Debug logger function
 */
function debugLog(category: string, message: string, ...args: any[]) {
    if (DEBUG_ENABLED) {
        // Only log connection messages if DEBUG_CONNECTIONS is enabled
        if (category === 'CONNECTION' && !DEBUG_CONNECTIONS) {
            return;
        }
        
        // Only log message-related content if DEBUG_MESSAGES is enabled
        if ((category === 'MESSAGE' || category === 'SEND') && !DEBUG_MESSAGES) {
            return;
        }
        
        console.log(`[${category}] ${message}`, ...args);
    }
}

/**
 * Class representing a message from the AMXX server
 */
export class Message {
    type: number;
    offset: number;
    buffer: Buffer;
    size: number;
    remainingSize: number;

    constructor(type: number, offset: number, size: number, buffer: Buffer) {
        this.type = type;
        this.offset = offset;
        this.buffer = buffer;
        this.size = size;
        this.remainingSize = buffer.length - offset - size;
    }

    /**
     * Read a 32-bit integer from the message
     */
    readInt(): number {
        const value = this.buffer.readInt32LE(this.offset);
        this.offset += 4;
        return value;
    }

    /**
     * Read a byte from the message
     */
    readByte(): number {
        const value = this.buffer.readInt8(this.offset);
        this.offset += 1;
        return value;
    }

    /**
     * Read a boolean value from the message
     */
    readBool(): boolean {
        return this.readInt() !== 0;
    }

    /**
     * Read a string from the message
     */
    readString(): string {
        let num = this.readInt();
        const ucs2 = num < 0;
        
        if (ucs2) {
            num = -num;
        }

        if (ucs2) {
            let str = this.buffer.toString("utf16le", this.offset, this.offset + num * 2);
            this.offset += num * 2;
            if (str[str.length - 1] === '\0') {
                str = str.substring(0, str.length - 1);
            }
            return str;
        } else {
            let str = this.buffer.toString("utf8", this.offset, this.offset + num);
            this.offset += num;
            if (str[str.length - 1] === '\0') {
                str = str.substring(0, str.length - 1);
            }
            return str;
        }
    }

    /**
     * Get debug representation of the message
     */
    toString(): string {
        const typeName = MessageTypeNames[this.type] || `Unknown(${this.type})`;
        return `Message(type=${typeName}, size=${this.size})`;
    }
}

/**
 * Create a buffer containing a 32-bit integer
 */
function writeInt(value: number): Buffer {
    const newBuffer = Buffer.alloc(4);
    newBuffer.writeInt32LE(value, 0);
    return newBuffer;
}

/**
 * Create a buffer containing a string
 */
function writeString(str: string): Buffer {
    const newBuffer = Buffer.alloc(4);
    newBuffer.writeInt32LE(str.length + 1, 0);
    return Buffer.concat([newBuffer, Buffer.from(str + "\0", "binary")]);
}

// Buffer for incomplete messages
let pendingBuffer: Buffer = Buffer.alloc(0);

/**
 * Read messages from a buffer
 */
export function readMessages(buffer: Buffer): Array<Message> {
    const list: Array<Message> = [];

    pendingBuffer = Buffer.concat([pendingBuffer, buffer]);
    debugLog('MESSAGE', `Received data, total pending buffer size: ${pendingBuffer.length} bytes`);

    while (pendingBuffer.length >= 5) {
        let offset = 0;
        const msglen = pendingBuffer.readUInt32LE(offset);
        offset += 4;
        const msgtype = pendingBuffer.readInt8(offset);
        offset += 1;

        if (msglen <= pendingBuffer.length - offset) {
            const message = new Message(msgtype, offset, msglen, pendingBuffer);
            list.push(message);
            pendingBuffer = pendingBuffer.slice(offset + msglen);
            
            if (DEBUG_MESSAGES) {
                debugLog('MESSAGE', `Parsed ${message.toString()}`);
            }
        } else {
            debugLog('MESSAGE', `Incomplete message, waiting for more data (have ${pendingBuffer.length - offset}, need ${msglen})`);
            return list;
        }
    }

    if (pendingBuffer.length > 0) {
        debugLog('MESSAGE', `Remaining data in buffer (${pendingBuffer.length} bytes), waiting for more`);
    }

    return list;
}

// Socket connection to AMXX
let sock: Socket | null = null;
export let connected = false;
export let events = new EventEmitter();

// Reconnection settings
let connectionRetries = 0;
const MAX_RETRIES = 3;
const RETRY_DELAY = 1000; // ms

/**
 * Connect to the AMXX server
 */
export function connect() {
    const host = vscode.workspace.getConfiguration('sourcepawn-remote-debug').get("remoteHost") as string;
    const port = vscode.workspace.getConfiguration('sourcepawn-remote-debug').get("remotePort") as number;

    debugLog('CONNECTION', `Connecting to ${host}:${port}`);

    if (sock) {
        try {
            sock.destroy();
        } catch (e) {
            debugLog('CONNECTION', `Error destroying existing socket: ${e.message}`);
        }
    }
    
    sock = new Socket();
    connected = false; // Reset connection status until we're fully connected
    
    if (DEBUG_CONNECTIONS) {
        // Add more detailed connection logging for debugging
        sock.on('lookup', (err, address, family, host) => {
            debugLog('CONNECTION', `DNS lookup for ${host}: ${address} (${family})`);
        });
        
        sock.on('timeout', () => {
            debugLog('CONNECTION', 'Connection timeout');
        });
        
        sock.on('end', () => {
            debugLog('CONNECTION', 'Server ended the connection');
            connected = false;
        });
    }
    
    // Set longer timeout to prevent premature disconnection
    sock.setTimeout(30000);
    
    sock.connect(port, host, function() {
        debugLog('CONNECTION', 'Connection to srcds server established');
        
        if (DEBUG_CONNECTIONS && sock) { // Added null check
            const localAddr = sock.localAddress ? `${sock.localAddress}:${sock.localPort}` : 'unknown';
            const remoteAddr = sock.remoteAddress ? `${sock.remoteAddress}:${sock.remotePort}` : 'unknown';
            debugLog('CONNECTION', `Socket details - Local: ${localAddr}, Remote: ${remoteAddr}`);
        }
        
        // Reset retry counter on successful connection
        connectionRetries = 0;
        connected = true;
        events.emit("Connected");
    });

    sock.on("data", function(data: Buffer) {
        debugLog('SOCKET', `Received ${data.length} bytes`);
        
        const messages: Array<Message> = readMessages(data);
        for (const msg of messages) {
            if (DEBUG_MESSAGES) {
                debugLog('SOCKET', `Processing message: ${MessageTypeNames[msg.type] || msg.type}`);
            }
            
            switch (msg.type) {
                case MessageType.CallStack:
                    events.emit("CallStack", msg);
                    break;
                case MessageType.HasStopped:
                    events.emit("Stopped", msg);
                    break;
                case MessageType.HasContinued:
                    events.emit("Continued", msg);
                    break;
                case MessageType.Variables:
                    events.emit("Variables", msg);
                    break;
                case MessageType.Evaluate:
                    events.emit("Evaluate", msg);
                    break;
                case MessageType.SetBreakpoint:
                    events.emit("SetBreakpoint", msg);
                    break;
                case MessageType.SetVariable:
                    events.emit("SetVariable", msg);
                    break;
                default:
                    debugLog('SOCKET', `Unhandled message type: ${msg.type}`);
                    break;
            }
        }
    });

    sock.on("error", function(err) {
        debugLog('CONNECTION', `Socket error: ${err.message}`);
        if (DEBUG_CONNECTIONS) {
            debugLog('CONNECTION', `Error details: ${JSON.stringify(err)}`);
        }
        if (sock != null) {
            sock.destroy();
            connected = false;
            events.emit("Closed", err);
            
            // Try to reconnect if we haven't reached the maximum number of retries
            tryReconnect();
        }
    });

    sock.on("close", function(hadError) {
        if (DEBUG_CONNECTIONS && sock) { // Added null check
            debugLog('CONNECTION', `Connection closed${hadError ? ' due to transmission error' : ' normally'}`);
            debugLog('CONNECTION', `Socket state: ${sock.readyState}`);
        } else {
            debugLog('CONNECTION', 'Connection closed');
        }
        
        if (sock != null) {
            sock.destroy();
            connected = false;
            events.emit("Closed", hadError);
            
            // If connection was closed unexpectedly, try to reconnect
            if (hadError) {
                tryReconnect();
            }
        }
    });
}

/**
 * Try to reconnect to the server with exponential backoff
 */
function tryReconnect() {
    if (connectionRetries < MAX_RETRIES) {
        const delay = RETRY_DELAY * Math.pow(2, connectionRetries);
        debugLog('CONNECTION', `Attempting to reconnect in ${delay}ms (attempt ${connectionRetries + 1}/${MAX_RETRIES})`);
        
        setTimeout(() => {
            connectionRetries++;
            connect();
        }, delay);
    } else {
        debugLog('CONNECTION', `Maximum reconnection attempts (${MAX_RETRIES}) reached. Giving up.`);
        // Reset counter for future connection attempts
        connectionRetries = 0;
    }
}

/**
 * Disconnect from the AMXX server
 */
export function disconnect() {
    debugLog('CONNECTION', 'Disconnecting from server');
    
    if (DEBUG_CONNECTIONS && sock) { // Added null check
        debugLog('CONNECTION', `Disconnecting from ${sock.remoteAddress}:${sock.remotePort}`);
        debugLog('CONNECTION', `Connection was active for ${sock.bytesRead} bytes read, ${sock.bytesWritten} bytes written`);
    }
    
    try {
        sendDisconnect();
    } catch (e) {
        debugLog('CONNECTION', `Error sending disconnect message: ${e.message}`);
    }
    
    if (sock) {
        sock.destroy();
    }
    
    connected = false;
}

/**
 * Send a message to the AMXX server
 */
function sendMessage(messageType: MessageType, payload: Buffer = Buffer.alloc(0), extraByte?: number) {
    if (!sock) {
        debugLog('ERROR', 'Cannot send message, socket not initialized');
        return false;
    }
    
    if (sock.destroyed) {
        debugLog('ERROR', 'Cannot send message, socket is destroyed');
        return false;
    }
    
    if (!connected) {
        debugLog('ERROR', 'Cannot send message, not connected (socket exists but not marked as connected)');
        
        // Try to reconnect if socket exists but we're not marked as connected
        if (!sock.destroyed && sock.readyState === 'open') {
            debugLog('CONNECTION', 'Socket appears to be open but not marked as connected, marking as connected');
            connected = true;
        } else {
            return false;
        }
    }
    
    if (DEBUG_CONNECTIONS) {
        // Log socket state before sending
        const state = sock.destroyed ? 'destroyed' : 
                     sock.connecting ? 'connecting' : 
                     sock.pending ? 'pending' : 'established';
        debugLog('CONNECTION', `Socket state before sending: ${state}, readyState: ${sock.readyState}`);
    }

    let headerSize = 5; // 4 bytes for length + 1 byte for type
    let headerBuffer = Buffer.alloc(headerSize);
    
    // If we have an extra byte, add it to the header
    if (extraByte !== undefined) {
        headerSize++;
        headerBuffer = Buffer.alloc(headerSize);
        headerBuffer.writeUInt8(extraByte, 5);
    }
    
    // Write message type
    headerBuffer.writeUInt8(messageType, 4);
    
    // Combine header and payload
    const fullMessage = Buffer.concat([headerBuffer, payload]);
    
    // Write message length (excluding the length field itself)
    fullMessage.writeUInt32LE(fullMessage.length - 4, 0);
    
    if (DEBUG_MESSAGES) {
        debugLog('SEND', `Sending message: ${MessageTypeNames[messageType] || messageType} (${fullMessage.length} bytes)`);
    }
    
    try {
        return sock.write(fullMessage, (err) => {
            if (err) {
                debugLog('ERROR', `Error writing to socket: ${err.message}`);
                
                if (DEBUG_CONNECTIONS) {
                    debugLog('CONNECTION', `Write error details: ${JSON.stringify(err)}`);
                }
                
                // The socket had an error, mark as disconnected
                connected = false;
                return false;
            }
            
            if (DEBUG_CONNECTIONS) {
                debugLog('CONNECTION', `Message sent successfully (${fullMessage.length} bytes)`);
            }
        });
    } catch (e) {
        debugLog('ERROR', `Exception sending message: ${e.message}`);
        connected = false;
        return false;
    }
}

/**
 * Send a pause request
 */
export function sendPause() {
    debugLog('DEBUG', 'Sending pause request');
    return sendMessage(MessageType.Pause, Buffer.alloc(0), 2);
}

/**
 * Send a continue request
 */
export function sendContinue() {
    debugLog('DEBUG', 'Sending continue request');
    return sendMessage(MessageType.Continue, Buffer.alloc(0), 0);
}

/**
 * Send a request for the call stack
 */
export function sendRequestCallStack() {
    debugLog('DEBUG', 'Requesting call stack');
    return sendMessage(MessageType.RequestCallStack);
}

/**
 * Send a disconnect request
 */
export function sendDisconnect() {
    debugLog('DEBUG', 'Sending disconnect request');
    return sendMessage(MessageType.Disconnect);
}

export function sendStartDebugging(filename: string) {
    debugLog('DEBUG', `Starting debugging for file: ${filename}`);
    
    // Ensure we have a connection before trying to send
    if (!isConnected()) {
        debugLog('CONNECTION', `Not connected when trying to start debugging for ${filename}, attempting to reconnect...`);
        
        // Set up a promise to wait for the connection
        const connectionPromise = new Promise<void>((resolve, reject) => {
            const timeout = setTimeout(() => {
                events.removeListener('Connected', onConnect);
                reject(new Error('Connection timeout'));
            }, 5000);
            
            const onConnect = () => {
                clearTimeout(timeout);
                events.removeListener('Connected', onConnect);
                resolve();
            };
            
            events.once('Connected', onConnect);
            connect();
        });
        
        // Return the result of the promise chain
        return connectionPromise
            .then(() => {
                debugLog('CONNECTION', 'Reconnected successfully, now sending start debugging message');
                return sendMessage(MessageType.RequestFile, writeString(filename));
            })
            .catch(err => {
                debugLog('ERROR', `Failed to reconnect: ${err.message}`);
                return false;
            });
    }
    
    return sendMessage(MessageType.RequestFile, writeString(filename));
}

/**
 * Send a stop debugging request
 */
export function sendStopDebugging() {
    debugLog('DEBUG', 'Stopping debugging');
    return sendMessage(MessageType.StopDebugging);
}

/**
 * Clear all breakpoints for a file
 */
export function clearBreakpoints(pathname: string) {
    debugLog('DEBUG', `Clearing breakpoints for: ${pathname}`);
    return sendMessage(MessageType.ClearBreakpoints, writeString(pathname));
}

/**
 * Set a breakpoint
 */
export function setBreakpoint(id: number, pathname: string, line: number) {
    debugLog('DEBUG', `Setting breakpoint ${id} at ${pathname}:${line}`);
    
    const payload = Buffer.concat([
        writeString(pathname), 
        writeInt(line), 
        writeInt(id)
    ]);
    
    return sendMessage(MessageType.SetBreakpoint, payload);
}

/**
 * Send a step-in request
 */
export function sendStepIn() {
    debugLog('DEBUG', 'Stepping in');
    return sendMessage(MessageType.StepIn, Buffer.alloc(0), 3);
}

/**
 * Send a step-over request
 */
export function sendStepOver() {
    debugLog('DEBUG', 'Stepping over');
    return sendMessage(MessageType.StepOver, Buffer.alloc(0), 4);
}

/**
 * Send a step-out request
 */
export function sendStepOut() {
    debugLog('DEBUG', 'Stepping out');
    return sendMessage(MessageType.StepOut, Buffer.alloc(0), 5);
}

/**
 * Send a request to set a variable value
 * This is used in the debug UI to modify variable values during debugging
 */
export function sendRequestSetVariable(variable: string, value: string, index: number) {
    debugLog('DEBUG', `Setting variable '${variable}' to '${value}' at index ${index}`);
    
    const payload = Buffer.concat([
        writeString(variable), 
        writeString(value), 
        writeInt(index)
    ]);
    
    return sendMessage(MessageType.RequestSetVariable, payload);
}

/**
 * Send a request for variables
 */
export function sendRequestVariables(path: string) {
    debugLog('DEBUG', `Requesting variables for path: ${path}`);
    return sendMessage(MessageType.RequestVariables, writeString(path));
}

/**
 * Send a request to evaluate an expression
 */
export function sendRequestEvaluate(path: string, frameId: number) {
    debugLog('DEBUG', `Evaluating '${path}' in frame ${frameId}`);
    
    const payload = Buffer.concat([
        writeString(path), 
        writeInt(frameId)
    ]);
    
    return sendMessage(MessageType.RequestEvaluate, payload);
}

/**
 * Check the connection status
 */
export function isConnected(): boolean {
    if (!sock) {
        return false;
    }
    
    // Check both our connected flag and the socket state
    return connected && 
           !sock.destroyed && 
           sock.readyState === 'open';
}

/**
 * Get connection information
 */
export function getConnectionInfo() {
    if (!connected || !sock) {
        return { connected: false };
    }
    
    const info = {
        connected: connected,
        localAddress: sock.localAddress,
        localPort: sock.localPort,
        remoteAddress: sock.remoteAddress,
        remotePort: sock.remotePort
    };
    
    if (DEBUG_CONNECTIONS) {
        // Add extended information when DEBUG_CONNECTIONS is enabled
        return {
            ...info,
            bytesRead: sock.bytesRead,
            bytesWritten: sock.bytesWritten,
            readyState: sock.readyState,
            pending: sock.pending,
            connecting: sock.connecting,
            destroyed: sock.destroyed,
            timeout: sock.timeout,
            pendingBufferSize: pendingBuffer.length
        };
    }
    
    return info;
}

export function sendRequestSendVariable(name: string, value: string, index: number) {
	throw new Error('Function not implemented.');
}
