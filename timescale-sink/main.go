/*
 * This is a simple MQTT subscriber that publishes messages to a timescale table
 * Set the following environment variables:
 * - BROKER_URI: MQTT broker URI
 * - CLIENT_ID: MQTT client ID (optional, defaults to hostname)
 * - DATABASE_URL: Timescale database URL
 */

package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"os"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	pgx "github.com/jackc/pgx/v5"
)

type Payload struct {
	Wifi       int32   // wifi signal strength
	Rco2       int32   // CO2
	Pm01       int32   // PM1.0
	Pm25       int32   // PM2.5
	Pm10       int32   // PM10
	Pm03PCount int32   // PM0.3
	TvocIndex  int32   // TVOC
	NoxIndex   int32   // NOx
	Atmp       float32 // temperature
	Rhum       int32   // humidity
	Boot       int32   // loopCount
}

func prettyPrint(payload Payload) {
	fmt.Printf("Wifi: %d ", payload.Wifi)
	fmt.Printf("CO2: %d ", payload.Rco2)
	fmt.Printf("PM1.0: %d ", payload.Pm01)
	fmt.Printf("PM2.5: %d ", payload.Pm25)
	fmt.Printf("PM10: %d ", payload.Pm10)
	fmt.Printf("PM0.3: %d ", payload.Pm03PCount)
	fmt.Printf("TVOC: %d ", payload.TvocIndex)
	fmt.Printf("NOx: %d ", payload.NoxIndex)
	fmt.Printf("Temperature: %f ", payload.Atmp)
	fmt.Printf("Humidity: %d ", payload.Rhum)
	fmt.Printf("Boot: %d\n", payload.Boot)
}

func createTable(conn *pgx.Conn) error {
	// create table if it doesn't exist
	_, err := conn.Exec(
		context.Background(),
		`
		CREATE TABLE IF NOT EXISTS sensor_measurements (
			time TIMESTAMPTZ NOT NULL DEFAULT NOW(),
			sensor_id TEXT NOT NULL,
			wifi INT,
			co2 INT,
			pm01 INT,
			pm25 INT,
			pm10 INT,
			pm03pcount INT,
			tvoc INT,
			nox INT,
			temperature FLOAT,
			humidity INT,
			boot INT
		);

		SELECT create_hypertable('sensor_measurements', 'time');
	`)
	if err == nil {
		fmt.Println("Created table sensor_measurements")
	}
	return err
}

func callback(client mqtt.Client, msg mqtt.Message, publishChannel chan<- mqtt.Message) {
	publishChannel <- msg
	return
}

func insertDB(topic string, conn *pgx.Conn, payload Payload) {
	_, err := conn.Exec(
		context.Background(),
		`INSERT INTO sensor_measurements (
			sensor_id, 
			wifi,
			co2,
			pm01,
			pm25,
			pm10,
			pm03pcount,
			tvoc,
			nox,
			temperature,
			humidity,
			boot
		)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)`,
		topic,
		payload.Wifi,
		payload.Rco2,
		payload.Pm01,
		payload.Pm25,
		payload.Pm10,
		payload.Pm03PCount,
		payload.TvocIndex,
		payload.NoxIndex,
		payload.Atmp,
		payload.Rhum,
		payload.Boot,
	)
	if err != nil {
		fmt.Println("Unable to insert into database:", err)
	} else {
		prettyPrint(payload)
	}
}

func createMQTTClient() mqtt.Client {
	brokerUri := os.Getenv("BROKER_URI")
	if brokerUri == "" {
		fmt.Println("BROKER_URI environment variable must be set")
		os.Exit(1)
	}
	clientId := os.Getenv("CLIENT_ID")
	if clientId == "" {
		clientId, _ = os.Hostname()
	}
	fmt.Println("MQTT Client ID:", clientId)

	opts := mqtt.NewClientOptions().AddBroker(brokerUri).SetClientID(clientId)
	opts.SetKeepAlive(2 * time.Second)
	return mqtt.NewClient(opts)
}

func main() {
	mqttClient := createMQTTClient()
	if token := mqttClient.Connect(); token.Wait() && token.Error() != nil {
		fmt.Println(token.Error())
		os.Exit(1)
	}

	dbconn, err := pgx.Connect(context.Background(), os.Getenv("DATABASE_URL"))
	if err != nil {
		fmt.Println("Unable to connect to database:", err)
		os.Exit(1)
	}
	createTable(dbconn)

	publishChannel := make(chan mqtt.Message)
	if token := mqttClient.Subscribe("/office", 0, func(client mqtt.Client, msg mqtt.Message) {
		callback(client, msg, publishChannel)
	}); token.Wait() && token.Error() != nil {
		fmt.Println(token.Error())
		os.Exit(1)
	}

	go func() {
		for msg := range publishChannel {
			// message is in the Payload struct binary format
			// unmarshal it into a struct
			var payload Payload
			b := msg.Payload()
			err := binary.Read(bytes.NewReader(b), binary.LittleEndian, &payload)
			if err != nil {
				fmt.Println("binary.Read failed:", err)
			}
			// if connection is lost, try to reconnect
			var skipInsert bool = false
			if dbconn.IsClosed() {
				dbconn, err = pgx.Connect(context.Background(), os.Getenv("DATABASE_URL"))
				if err != nil {
					fmt.Println("Unable to connect to database:", err)
					skipInsert = true
				}
			}
			if skipInsert {
				continue
			}
			insertDB(msg.Topic(), dbconn, payload)
		}
	}()

	mainChannel := make(chan os.Signal)
	fmt.Println("Waiting for messages... (Ctrl-C to exit)")
	<-mainChannel
}
