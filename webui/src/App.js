import React, { useCallback, useEffect, useState, useRef } from 'react';

import './App.css';

import Alert from 'react-bootstrap/Alert'
import Navbar from 'react-bootstrap/Navbar'
import Nav from 'react-bootstrap/Nav'
import NavDropdown from 'react-bootstrap/NavDropdown'

import Container from 'react-bootstrap/Container'
import Row from 'react-bootstrap/Row'
import Col from 'react-bootstrap/Col'

import Form from 'react-bootstrap/Form'
import Button from 'react-bootstrap/Button'
import ToggleButton from 'react-bootstrap/ToggleButton'

import {
  BrowserRouter as Router,
  Switch,
  Route,
  Link
} from "react-router-dom";

// Maximum number of historical sensor values to retain
const MAX_SIZE = 1000;

// Returned `defaults` property will be undefined if the defaults are loading or reloading.
// Call `reloadDefaults` to clear the defaults and reload from the server.
function useDefaults() {
  const [defaults, setDefaults] = useState(undefined);

  const reloadDefaults = useCallback(() => setDefaults(undefined), [setDefaults]);

  // Load defaults at mount and reload any time they are cleared.
  useEffect(() => {
    let cleaningUp = false;
    let timeoutId = 0;

    const getDefaults = () => {
      clearTimeout(timeoutId);
      fetch('/defaults').then(res => res.json()).then(data => {
        if (!cleaningUp) {
          setDefaults(data);
        }
      }).catch(reason => {
        if (!cleaningUp) {
          timeoutId = setTimeout(getDefaults, 1000);
        }
      });
    }

    if (!defaults) {
      getDefaults();
    }

    return () => {
      cleaningUp = true;
      clearTimeout(timeoutId);
    };
  }, [defaults]);

  return { defaults, reloadDefaults };
}

// returns { emit, isWsReady, webUIDataRef, wsCallbacksRef }
// webUIDataRef tracks values as well as thresholds.
// Use emit to send events to the server.
// isWsReady indicates that the websocket is connected and has received
// its first values reading from the server.

// onCloseWs is called when the websocket closes for any reason,
// unless the hooks is already doing effect cleanup.

// defaults is expected to be undefined if defaults are loading or reloading.
// The expectation is that onCloseWs is used to reload default thresholds
// and provide new ones to trigger a new connection.
function useWsConnection({ defaults, onCloseWs }) {
  const [isWsReady, setIsWsReady] = useState(false);

  // Some values such as sensor readings are stored in a mutable array in a ref so that
  // they are not subject to the React render cycle, for performance reasons.
  const webUIDataRef = useRef({
    slots: {},
  });

  const wsRef = useRef();
  const wsCallbacksRef = useRef({});

  const emit = useCallback((msg) => {
    // App should wait for isWsReady to send messages.
    if (!wsRef.current || !isWsReady) {
      throw new Error("emit() called when isWsReady !== true.");
    }

    wsRef.current.send(JSON.stringify(msg));
  }, [isWsReady, wsRef]);

  wsCallbacksRef.current.values = function(msg) {
    const slotData = webUIDataRef.current.slots[msg.slot];
    if (!slotData) {
      return;
    }
    if (slotData.curValues.length < MAX_SIZE) {
      slotData.curValues.push(msg.values);
    } else {
      slotData.curValues[slotData.oldest] = msg.values;
      slotData.oldest = (slotData.oldest + 1) % MAX_SIZE;
    }
  };

  wsCallbacksRef.current.thresholds = function(msg) {
    const slotData = webUIDataRef.current.slots[msg.slot];
    if (!slotData) {
      return;
    }
    // Modify thresholds array in place instead of replacing it
    // so that animation loops can have a stable reference.
    slotData.curThresholds.length = 0;
    slotData.curThresholds.push(...msg.thresholds);
  };

  useEffect(() => {
    let cleaningUp = false;
    if (!defaults) {
      // If defaults are loading or reloading, don't connect.
      return;
    }

    // Ensure values history reset and default thresholds are set for each slot.
    webUIDataRef.current.slots = {};
    Object.keys(defaults.slots).forEach((slot) => {
      const thresholds = defaults.slots[slot].thresholds || [];
      webUIDataRef.current.slots[slot] = {
        curValues: [new Array(thresholds.length).fill(0)],
        oldest: 0,
        curThresholds: [...thresholds],
      };
    });

    const ws = new WebSocket('ws://' + window.location.host + '/ws');
    wsRef.current = ws;

    ws.addEventListener('open', function(ev) {
      setIsWsReady(true);
    });
    
    ws.addEventListener('error', function(ev) {
      ws.close();
    });

    ws.addEventListener('close', function(ev) {
      if (!cleaningUp) {
        onCloseWs();
      }
    });

    ws.addEventListener('message', function(ev) {
      const data = JSON.parse(ev.data)
      const action = data[0];
      const msg = data[1];

      if (wsCallbacksRef.current[action]) {
        wsCallbacksRef.current[action](msg);
      }
    });

    return () => {
      cleaningUp = true;
      setIsWsReady(false);
      ws.close();
    };
  }, [defaults, onCloseWs]);

  return { emit, isWsReady, webUIDataRef, wsCallbacksRef };
}

// An interactive display of the current values obtained by the backend.
// Also has functionality to manipulate thresholds.
function ValueMonitor(props) {
  const { emit, slot, index, webUIDataRef } = props;
  const thresholdLabelRef = React.useRef(null);
  const valueLabelRef = React.useRef(null);
  const canvasRef = React.useRef(null);

  const getSlotData = useCallback(() => {
    return webUIDataRef.current.slots[slot] || { curValues: [], oldest: 0, curThresholds: [] };
  }, [slot, webUIDataRef]);

  const EmitValue = useCallback((val) => {
    // Send back all the thresholds instead of a single value per sensor. This is in case
    // the server restarts where it would be nicer to have all the values in sync.
    // Still send back the index since we want to update only one value at a time
    // to the microcontroller.
    const slotData = getSlotData();
    emit(['update_threshold', slot, slotData.curThresholds, index]);
  }, [emit, getSlotData, index, slot])

  function Decrement(e) {
    const slotData = getSlotData();
    const curThresholds = slotData.curThresholds;
    const val = curThresholds[index] - 1;
    if (val >= 0) {
      curThresholds[index] = val;
      EmitValue(val);
    }
  }

  function Increment(e) {
    const slotData = getSlotData();
    const curThresholds = slotData.curThresholds;
    const val = curThresholds[index] + 1;
    if (val <= 1023) {
      curThresholds[index] = val
      EmitValue(val);
    }
  }

  useEffect(() => {
    let requestId;

    const canvas = canvasRef.current;
    const ctx = canvas.getContext('2d');

    function getMousePos(canvas, e) {
      const rect = canvas.getBoundingClientRect();
      const dpi = window.devicePixelRatio || 1;
      return {
        x: (e.clientX - rect.left) * dpi,
        y: (e.clientY - rect.top) * dpi
      };
    }

    function getTouchPos(canvas, e) {
      const rect = canvas.getBoundingClientRect();
      const dpi = window.devicePixelRatio || 1;
      return {
        x: (e.targetTouches[0].pageX - rect.left - window.pageXOffset) * dpi,
        y: (e.targetTouches[0].pageY - rect.top - window.pageYOffset) * dpi
      };
    }
    // Change the thresholds while dragging, but only emit on release.
    let is_drag = false;

    // Mouse Events
    canvas.addEventListener('mousedown', function(e) {
      const curThresholds = getSlotData().curThresholds;
      let pos = getMousePos(canvas, e);
      curThresholds[index] = Math.floor(1023 - pos.y/canvas.height * 1023);
      is_drag = true;
    });

    canvas.addEventListener('mouseup', function(e) {
      const curThresholds = getSlotData().curThresholds;
      EmitValue(curThresholds[index]);
      is_drag = false;
    });

    canvas.addEventListener('mousemove', function(e) {
      if (is_drag) {
        const curThresholds = getSlotData().curThresholds;
        let pos = getMousePos(canvas, e);
        curThresholds[index] = Math.floor(1023 - pos.y/canvas.height * 1023);
      }
    });

    // Touch Events
    canvas.addEventListener('touchstart', function(e) {
      const curThresholds = getSlotData().curThresholds;
      let pos = getTouchPos(canvas, e);
      curThresholds[index] = Math.floor(1023 - pos.y/canvas.height * 1023);
      is_drag = true;
    });

    canvas.addEventListener('touchend', function(e) {
      const curThresholds = getSlotData().curThresholds;
      // We don't need to set the curThreshold as it's already updated within the
      // touchstart/touchmove events.
      EmitValue(curThresholds[index]);
      is_drag = false;
    });

    canvas.addEventListener('touchmove', function(e) {
      if (is_drag) {
        const curThresholds = getSlotData().curThresholds;
        let pos = getTouchPos(canvas, e);
        curThresholds[index] = Math.floor(1023 - pos.y/canvas.height * 1023);
      }
    });

    const setDimensions = () => {
      // Adjust DPI so that all the edges are smooth during scaling.
      const dpi = window.devicePixelRatio || 1;

      canvas.width = canvas.clientWidth * dpi;
      canvas.height = canvas.clientHeight * dpi;
    };

    setDimensions();
    window.addEventListener('resize', setDimensions);

    // This is default React CSS font style.
    const bodyFontFamily = window.getComputedStyle(document.body).getPropertyValue("font-family");
    const valueLabel = valueLabelRef.current;
    const thresholdLabel = thresholdLabelRef.current;

    // Cap animation to 60 FPS (with slight leeway because monitor refresh rates are not exact).
    const minFrameDurationMs = 1000 / 60.1;
    let previousTimestamp;

    const render = (timestamp) => {
      const slotData = getSlotData();
      const curValues = slotData.curValues;
      const curThresholds = slotData.curThresholds;
      const oldest = slotData.oldest;

      if (curValues.length === 0 || curThresholds.length <= index) {
        requestId = requestAnimationFrame(render);
        return;
      }

      if (previousTimestamp && (timestamp - previousTimestamp) < minFrameDurationMs) {
        requestId = requestAnimationFrame(render);
        return;
      }
      previousTimestamp = timestamp;

      // Get the latest value. This is either last element in the list, or based off of
      // the circular array.
      let currentValue = 0;
      if (curValues.length < MAX_SIZE) {
        currentValue = curValues[curValues.length-1][index];
      } else {
        currentValue = curValues[((oldest - 1) % MAX_SIZE + MAX_SIZE) % MAX_SIZE][index];
      }

      // Add background fill.
      let grd = ctx.createLinearGradient(canvas.width/2, 0, canvas.width/2 ,canvas.height);
      if (currentValue >= curThresholds[index]) {
        grd.addColorStop(0, 'lightblue');
        grd.addColorStop(1, 'blue');
      } else {
        grd.addColorStop(0, 'lightblue');
        grd.addColorStop(1, 'gray');
      }
      ctx.fillStyle = grd;
      ctx.fillRect(0, 0, canvas.width, canvas.height);

      // Cur Value Label
      valueLabel.innerText = currentValue;

      // Bar
      const maxHeight = canvas.height;
      const position = Math.round(maxHeight - currentValue / 1023 * maxHeight);
      grd = ctx.createLinearGradient(canvas.width / 2, canvas.height, canvas.width / 2, position);
      grd.addColorStop(0, 'orange');
      grd.addColorStop(1, 'red');
      ctx.fillStyle = grd;
      ctx.fillRect(canvas.width / 4, position, canvas.width / 2, canvas.height);

      // Threshold Line
      const threshold_height = 3
      const threshold_pos = (1023 - curThresholds[index]) / 1023 * canvas.height;
      ctx.fillStyle = "black";
      ctx.fillRect(
          0, threshold_pos - Math.floor(threshold_height / 2), canvas.width, threshold_height);

      // Threshold Label
      thresholdLabel.innerText = curThresholds[index];
      ctx.font = "30px " + bodyFontFamily;
      ctx.fillStyle = "black";
      if (curThresholds[index] > 990) {
        ctx.textBaseline = 'top';
      } else {
        ctx.textBaseline = 'bottom';
      }
      ctx.fillText(curThresholds[index].toString(), 0, threshold_pos + threshold_height + 1);

      requestId = requestAnimationFrame(render);
    };

    render();

    return () => {
      cancelAnimationFrame(requestId);
      window.removeEventListener('resize', setDimensions);
    };
  }, [EmitValue, getSlotData, index, webUIDataRef]);

  return(
    <Col className="ValueMonitor-col">
      <div className="ValueMonitor-buttons">
        <Button variant="light" size="sm" onClick={Decrement}><b>-</b></Button>
        <span> </span>
        <Button variant="light" size="sm" onClick={Increment}><b>+</b></Button>
      </div>
      <Form.Label className="ValueMonitor-label" ref={thresholdLabelRef}>0</Form.Label>
      <Form.Label className="ValueMonitor-label" ref={valueLabelRef}>0</Form.Label>
      <canvas
        className="ValueMonitor-canvas"
        ref={canvasRef}
      />
    </Col>
  );
}

function ValueMonitors(props) {
  const { numSensors, containerHeight } = props;
  return (
    <header className="App-header">
      <Container fluid style={{border: '1px solid white', height: containerHeight || '100vh'}}>
        <Row className="ValueMonitor-row">
          {props.children}
        </Row>
      </Container>
      <style>
        {`
        .ValueMonitor-col {
          width: ${100 / numSensors}%;
        }
        /* 15 + 15 is left and right padding (from bootstrap col class). */
        /* 75 is the minimum desired width of the canvas. */
        /* If there is not enough room for all columns and padding to fit, reduce padding. */
        @media (max-width: ${numSensors * (15 + 15 + 75)}px) {
          .ValueMonitor-col {
            padding-left: 1px;
            padding-right: 1px;
          }
        }
        `}
      </style>
    </header>
  );
}

function Plot(props) {
  const canvasRef = React.useRef(null);
  const { numSensors, slot, webUIDataRef, containerHeight } = props;
  const [display, setDisplay] = useState(new Array(numSensors).fill(true));
  // `buttonNames` is only used if the number of sensors matches the number of button names.
  const buttonNames = ['Left', 'Down', 'Up', 'Right'];

  const getSlotData = useCallback(() => {
    return webUIDataRef.current.slots[slot] || { curValues: [], oldest: 0, curThresholds: [] };
  }, [slot, webUIDataRef]);

  // Color values for sensors
  const degreesPerSensor = 360 / numSensors;
  const colors = [...Array(numSensors)].map((_, i) => `hsl(${degreesPerSensor * i}, 100%, 40%)`);
  const darkColors = [...Array(numSensors)].map((_, i) => `hsl(${degreesPerSensor * i}, 100%, 35%)`);

  useEffect(() => {
    let requestId;
    const canvas = canvasRef.current;
    const ctx = canvas.getContext('2d');

    const setDimensions = () => {
      // Adjust DPI so that all the edges are smooth during scaling.
      const dpi = window.devicePixelRatio || 1;

      canvas.width = canvas.clientWidth * dpi;
      canvas.height = canvas.clientHeight * dpi;
    };

    setDimensions();
    window.addEventListener('resize', setDimensions);

    // This is default React CSS font style.
    const bodyFontFamily = window.getComputedStyle(document.body).getPropertyValue("font-family");

    function drawDashedLine(pattern, spacing, y, width) {
      ctx.beginPath();
      ctx.setLineDash(pattern);
      ctx.moveTo(spacing, y);
      ctx.lineTo(width, y);
      ctx.stroke();
    }

    // Cap animation to 60 FPS (with slight leeway because monitor refresh rates are not exact).
    const minFrameDurationMs = 1000 / 60.1;
    let previousTimestamp;

    const render = (timestamp) => {
      const slotData = getSlotData();
      const curValues = slotData.curValues;
      const curThresholds = slotData.curThresholds;
      const oldest = slotData.oldest;

      if (curValues.length === 0) {
        requestId = requestAnimationFrame(render);
        return;
      }

      if (previousTimestamp && (timestamp - previousTimestamp) < minFrameDurationMs) {
        requestId = requestAnimationFrame(render);
        return;
      }
      previousTimestamp = timestamp;

      // Add background fill.
      ctx.fillStyle = "#f8f9fa";
      ctx.fillRect(0, 0, canvas.width, canvas.height);

      // Border
      const spacing = 10;
      const box_width = canvas.width-spacing*2;
      const box_height = canvas.height-spacing*2
      ctx.strokeStyle = 'darkgray';
      ctx.beginPath();
      ctx.rect(spacing, spacing, box_width, box_height);
      ctx.stroke();

      // Draw the divisions in the plot.
      // Major Divisions will be 2 x minor_divison.
      const minor_division = 100;
      for (let i = 1; i*minor_division < 1023; ++i) {
        const pattern = i % 2 === 0 ? [20, 5] : [5, 10];
        drawDashedLine(pattern, spacing,
          box_height-(box_height * (i*minor_division)/1023) + spacing, box_width + spacing);
      }

      // Plot the line graph for each of the sensors.
      const px_per_div = box_width/MAX_SIZE;
      let plot_nums = 0;
      for (let i = 0; i < numSensors; ++i) {
        if (display[i]) {
          ++plot_nums;
        }
      }
      let k = -1;
      for (let i = 0; i < numSensors; ++i) {
        if (display[i]) {
          ++k;
          ctx.beginPath();
          ctx.setLineDash([]);
          ctx.strokeStyle = colors[i];
          ctx.lineWidth = 2;
          for (let j = 0; j < MAX_SIZE; ++j) {
            if (j === curValues.length) { break; }
            let y_value = (
                box_height
                - box_height * curValues[(j + oldest) % MAX_SIZE][i] / 1023 / plot_nums
                - k / plot_nums * box_height
                + spacing);
            if (j === 0) {
              ctx.moveTo(spacing, y_value);
            } else {
              ctx.lineTo(px_per_div * j + spacing, y_value);
            }
          }
          ctx.stroke();
        }
      }

      // Display the current thresholds.
      k = -1;
      for (let i = 0; i < numSensors; ++i) {
        if (display[i]) {
          ++k;
          ctx.beginPath();
          ctx.setLineDash([]);
          ctx.strokeStyle = darkColors[i];
          ctx.lineWidth = 2;
          let y_value = (
              box_height
              - box_height * curThresholds[i] / 1023 / plot_nums
              - k / plot_nums * box_height
              + spacing);
          ctx.moveTo(spacing, y_value);
          ctx.lineTo(box_width + spacing, y_value);
          ctx.stroke();
        }
      }

      // Display the current value for each of the sensors.
      ctx.font = "30px " + bodyFontFamily;
      for (let i = 0; i < numSensors; ++i) {
        if (display[i]) {
          ctx.fillStyle = colors[i];
          if (curValues.length < MAX_SIZE) {
            ctx.fillText(curValues[curValues.length-1][i], 100 + i * 100, 100);
          } else {
            ctx.fillText(
              curValues[((oldest - 1) % MAX_SIZE + MAX_SIZE) % MAX_SIZE][i], 100 + i * 100, 100);
          }
        }
      }

      requestId = requestAnimationFrame(render);
    };

    render();

    return () => {
      cancelAnimationFrame(requestId);
      window.removeEventListener('resize', setDimensions);
    };
  }, [colors, darkColors, display, getSlotData, numSensors, slot, webUIDataRef]);

  const ToggleLine = (index) => {
    setDisplay(display => {
      const updated = [...display];
      updated[index] = !updated[index];
      return updated;
    });
  };

  const toggleButtons = [];
  for (let i = 0; i < numSensors; i++) {
    toggleButtons.push(
      <ToggleButton
        className="ToggleButton-plot-sensor"
        key={i}
        type="checkbox"
        checked={display[i]}
        variant={display[i] ? "light" : "secondary"}
        size="sm"
        onChange={() => ToggleLine(i)}
      >
        <b style={{color: display[i] ? darkColors[i] : "#f8f9fa"}}>
          {numSensors === buttonNames.length ? buttonNames[i] : i}
        </b>
      </ToggleButton>
    );
  }

  return (
    <header className="App-header">
      <Container fluid style={{border: '1px solid white', height: containerHeight || '100vh'}}>
        <Row>
          <Col style={{height: '12%', paddingTop: '1vh'}}>
            <span>Display: </span>
            {toggleButtons}
          </Col>
        </Row>
        <Row>
          <Col style={{height: '84%'}}>
            <canvas
              ref={canvasRef}
              style={{
                border: '1px solid white',
                width: '100%',
                height: '100%',
                touchAction: "none"
              }} />
          </Col>
        </Row>
      </Container>
    </header>
  );
}

function FSRWebUI(props) {
  const { emit, defaults, webUIDataRef, wsCallbacksRef } = props;
  const slotIds = Object.keys(defaults.slots || {}).sort();
  const [profilesBySlot, setProfilesBySlot] = useState(
    slotIds.reduce((acc, slot) => {
      acc[slot] = defaults.slots[slot].profiles || [];
      return acc;
    }, {})
  );
  const [activeProfileBySlot, setActiveProfileBySlot] = useState(
    slotIds.reduce((acc, slot) => {
      acc[slot] = defaults.slots[slot].cur_profile || '';
      return acc;
    }, {})
  );
  const [serialPortBySlot, setSerialPortBySlot] = useState(
    slotIds.reduce((acc, slot) => {
      acc[slot] = defaults.slots[slot].serial_port || '';
      return acc;
    }, {})
  );
  const [serialPortCandidatesBySlot, setSerialPortCandidatesBySlot] = useState(
    slotIds.reduce((acc, slot) => {
      acc[slot] = defaults.slots[slot].serial_port_candidates || [];
      return acc;
    }, {})
  );
  const [showThresholdsSavedAlertBySlot, setShowThresholdsSavedAlertBySlot] = useState(
    slotIds.reduce((acc, slot) => {
      acc[slot] = false;
      return acc;
    }, {})
  );
  const [serialPortErrorBySlot, setSerialPortErrorBySlot] = useState(
    slotIds.reduce((acc, slot) => {
      acc[slot] = '';
      return acc;
    }, {})
  );

  function getSlotData(slot) {
    return webUIDataRef.current.slots[slot] || { curThresholds: [] };
  }

  useEffect(() => {
    const wsCallbacks = wsCallbacksRef.current;

    wsCallbacks.get_profiles = function(msg) {
      setProfilesBySlot(prev => ({ ...prev, [msg.slot]: msg.profiles }));
    };
    wsCallbacks.get_cur_profile = function(msg) {
      setActiveProfileBySlot(prev => ({ ...prev, [msg.slot]: msg.cur_profile }));
    };
    wsCallbacks.thresholds_persisted = function(msg) {
      setShowThresholdsSavedAlertBySlot(prev => ({ ...prev, [msg.slot]: true }));
    };
    wsCallbacks.serial_port = function(msg) {
      setSerialPortBySlot(prev => ({ ...prev, [msg.slot]: msg.serial_port || '' }));
      if (msg.serial_port_candidates) {
        setSerialPortCandidatesBySlot(prev => ({
          ...prev,
          [msg.slot]: msg.serial_port_candidates,
        }));
      }
      setSerialPortErrorBySlot(prev => ({ ...prev, [msg.slot]: '' }));
    };
    wsCallbacks.serial_port_candidates = function(msg) {
      setSerialPortCandidatesBySlot(prev => ({
        ...prev,
        [msg.slot]: msg.serial_port_candidates || [],
      }));
    };
    wsCallbacks.serial_port_error = function(msg) {
      setSerialPortErrorBySlot(prev => ({ ...prev, [msg.slot]: msg.message || '' }));
    };

    return () => {
      delete wsCallbacks.get_profiles;
      delete wsCallbacks.get_cur_profile;
      delete wsCallbacks.thresholds_persisted;
      delete wsCallbacks.serial_port;
      delete wsCallbacks.serial_port_candidates;
      delete wsCallbacks.serial_port_error;
    };
  }, [wsCallbacksRef]);

  function AddProfile(e, slot) {
    // Only add a profile on the enter key.
    if (e.keyCode === 13) {
      emit(['add_profile', slot, e.target.value, getSlotData(slot).curThresholds]);
      // Reset the text box.
      e.target.value = "";
    }
    return false;
  }

  function RemoveProfile(e, slot, profileName) {
    // The X button is inside the Change Profile button,
    // so stop the event from bubbling up and triggering the ChangeProfile handler.
    e.stopPropagation();
    emit(['remove_profile', slot, profileName]);
  }

  function SaveThresholds(slot) {
    emit(['save_thresholds', slot]);
  }

  function ChangeProfile(slot, profileName) {
    emit(['change_profile', slot, profileName]);
  }

  function ChangeSerialPort(slot, e) {
    const nextPort = e.target.value;
    setSerialPortBySlot(prev => ({ ...prev, [slot]: nextPort }));
    setSerialPortErrorBySlot(prev => ({ ...prev, [slot]: '' }));
    emit(['set_serial_port', slot, nextPort]);
  }

  function RefreshSerialCandidates(slot) {
    emit(['refresh_serial_port_candidates', slot]);
  }

  return (
    <div className="App">
      <Router>
        <>
          <Navbar bg="light">
            <Navbar.Brand as={Link} to="/">FSR WebUI</Navbar.Brand>
            <Nav>
              <Nav.Item>
                <Nav.Link as={Link} to="/plot">Plot</Nav.Link>
              </Nav.Item>
            </Nav>
            <Nav className="ml-auto">
              {slotIds.map(slot => {
                const serialPort = serialPortBySlot[slot] || '';
                const serialOptions = [...(serialPortCandidatesBySlot[slot] || [])];
                const hasSelectedPortCandidate = serialOptions.some(option => option.path === serialPort);
                const serialSelectValue = hasSelectedPortCandidate ? serialPort : '';
                const profiles = profilesBySlot[slot] || [];
                const activeProfile = activeProfileBySlot[slot] || '';
                return (
                  <React.Fragment key={slot}>
                    <Button
                      style={{marginRight: '0.5rem'}}
                      onClick={() => SaveThresholds(slot)}
                    >
                      Save {slot.toUpperCase()}
                    </Button>
                    <NavDropdown alignRight title={`Serial ${slot.toUpperCase()}`} id={`serial-port-dropdown-${slot}`}>
                      <div style={{padding: "0.5rem", minWidth: "22rem"}}>
                        <Form.Group controlId={`serialPortSelect-${slot}`} style={{marginBottom: "0.5rem"}}>
                          <Form.Label style={{fontSize: "0.9rem", marginBottom: "0.25rem"}}>
                            Device Port
                          </Form.Label>
                          <Form.Control as="select" value={serialSelectValue} onChange={(e) => ChangeSerialPort(slot, e)}>
                            {serialOptions.length === 0 ? (
                              <option value="">No matching serial devices found</option>
                            ) : (
                              <>
                                {!hasSelectedPortCandidate && (
                                  <option value="">Select a device port</option>
                                )}
                                {serialOptions.map(option => (
                                  <option
                                    key={option.path}
                                    value={option.path}
                                    disabled={Boolean(option.in_use_by)}
                                  >
                                    {option.label}
                                    {option.in_use_by ? ` (used by ${option.in_use_by.toUpperCase()})` : ''}
                                  </option>
                                ))}
                              </>
                            )}
                          </Form.Control>
                        </Form.Group>
                        {serialPortErrorBySlot[slot] && (
                          <div style={{color: "#dc3545", fontSize: "0.85rem", marginBottom: "0.5rem"}}>
                            {serialPortErrorBySlot[slot]}
                          </div>
                        )}
                        {serialOptions.length === 0 && (
                          <div style={{color: "#6c757d", fontSize: "0.85rem", marginBottom: "0.5rem"}}>
                            対象機器のシリアルデバイスが見つかりません。接続後に Refresh devices を押してください。
                          </div>
                        )}
                        <Button variant="outline-secondary" size="sm" onClick={() => RefreshSerialCandidates(slot)}>
                          Refresh devices
                        </Button>
                      </div>
                    </NavDropdown>
                    <NavDropdown alignRight title={`Profile ${slot.toUpperCase()}`} id={`profile-dropdown-${slot}`}>
                      {profiles.map((profile) => (
                        <NavDropdown.Item
                          key={`${slot}-${profile}`}
                          style={{paddingLeft: "0.5rem"}}
                          onClick={() => ChangeProfile(slot, profile)}
                          active={profile === activeProfile}
                        >
                          <Button variant="light" onClick={(e) => RemoveProfile(e, slot, profile)}>X</Button>{' '}{profile}
                        </NavDropdown.Item>
                      ))}
                      <NavDropdown.Divider />
                      <Form inline onSubmit={(e) => e.preventDefault()}>
                        <Form.Control
                            onKeyDown={(e) => AddProfile(e, slot)}
                            style={{marginLeft: "0.5rem", marginRight: "0.5rem"}}
                            type="text"
                            placeholder={`New Profile (${slot.toUpperCase()})`} />
                      </Form>
                    </NavDropdown>
                  </React.Fragment>
                );
              })}
            </Nav>
          </Navbar>
          {slotIds.map(slot => (
            <Alert
              key={`save-alert-${slot}`}
              show={Boolean(showThresholdsSavedAlertBySlot[slot])}
              variant="success"
              dismissible
              onClose={() => setShowThresholdsSavedAlertBySlot(prev => ({ ...prev, [slot]: false }))}
            >
              Saved thresholds to {slot.toUpperCase()} device successfully!
            </Alert>
          ))}
        </>
        <Switch>
          <Route exact path="/">
            <Container fluid style={{padding: 0}}>
              <Row noGutters>
                {slotIds.map(slot => {
                  const numSensors = (defaults.slots[slot].thresholds || []).length;
                  return (
                    <Col key={`slot-monitor-${slot}`} xs={12} md={6}>
                      <div style={{background: '#f8f9fa', color: '#333', padding: '0.35rem 0.75rem', border: '1px solid #ddd'}}>
                        {slot.toUpperCase()} Monitor
                      </div>
                      <ValueMonitors numSensors={numSensors} containerHeight="46vh">
                        {[...Array(numSensors).keys()].map(index => (
                          <ValueMonitor
                            emit={emit}
                            slot={slot}
                            index={index}
                            key={`${slot}-monitor-${index}`}
                            webUIDataRef={webUIDataRef}
                          />)
                        )}
                      </ValueMonitors>
                    </Col>
                  );
                })}
              </Row>
            </Container>
          </Route>
          <Route path="/plot">
            <Container fluid style={{padding: 0}}>
              <Row noGutters>
                {slotIds.map(slot => {
                  const numSensors = (defaults.slots[slot].thresholds || []).length;
                  return (
                    <Col key={`slot-plot-${slot}`} xs={12} md={6}>
                      <div style={{background: '#f8f9fa', color: '#333', padding: '0.35rem 0.75rem', border: '1px solid #ddd'}}>
                        {slot.toUpperCase()} Plot
                      </div>
                      <Plot
                        numSensors={numSensors}
                        slot={slot}
                        webUIDataRef={webUIDataRef}
                        containerHeight="46vh"
                      />
                    </Col>
                  );
                })}
              </Row>
            </Container>
          </Route>
        </Switch>
      </Router>
    </div>
  );
}

function LoadingScreen() {
  return (
    <div style={{ color: "white", height: "100vh", width: "100vw" }}>
      <Navbar bg="light">
        <Navbar.Brand as={"span"} to="/">FSR WebUI</Navbar.Brand>
      </Navbar>
      <div style={{
        backgroundColor: "#282c34",
        border: "1px solid white",
        fontSize: "1.25rem",
        padding: "0.5rem 1rem",
        height: "96vh"
      }}>
        Connecting...
      </div>
    </div>
  );
}

function App() {
  const { defaults, reloadDefaults } = useDefaults();
  const {
    emit,
    isWsReady,
    webUIDataRef,
    wsCallbacksRef
  } = useWsConnection({ defaults, onCloseWs: reloadDefaults });

  if (defaults && isWsReady) {
    return (
      <FSRWebUI
        defaults={defaults}
        emit={emit}
        webUIDataRef={webUIDataRef}
        wsCallbacksRef={wsCallbacksRef}
      />
    );
  } else {
    return <LoadingScreen />
  }
}

export default App;
