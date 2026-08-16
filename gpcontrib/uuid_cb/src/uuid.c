/* The original file is taken from https://www.cbr.ru/ckki/assignment_unique_id/ */

/* =================================================================== *
 * Вариант реализации функции генерации первой части уникального       *
 * идентификатора договора (сделки) - универсального уникального       *
 * идентификатора в соответствии с указанием Банка России              *
 * "О правилах присвоения уникального идентификатора договора (сделки),*
 * по обязательствам из которого (из которой) формируется кредитная    *
 * история"                                                            *
 * =================================================================== */

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
int hRandom = -1;

#include "uuid.h"

bool generator_init(void);
bool GetRND(uint32_t *);
uint64_t GetTime(void);


// Инициализация
bool uuid_init(void) {
	hRandom = open("/dev/random", O_RDONLY);
	if (hRandom < 0) {
		return false;
	}

	// Инициализация платформо-независимой части
	return generator_init();
}

// Деинициализация (освобождение ресурсов)
void uuid_deinit(void) {
	close(hRandom);
	hRandom = -1;
}

// Получение 32-битного случайного числа
bool GetRND(uint32_t *rnd) {
	if (read(hRandom, rnd, sizeof(uint32_t)) < 0) {
		return false;
	}
	return true;
}

// Получение времени в 100-наносекундных интервалах от 15.10.1580
uint64_t GetTime(void) {
	struct timeval tp;

	if (gettimeofday(&tp, (struct timezone *)0) != 0) {
		return 0;
	}

	// Приводим время к 100-наносекундным интервалам от 15.10.1580
	return ((uint64_t)tp.tv_sec * 10000000) + ((uint64_t)(tp.tv_usec / 1000) * 10000) + 0x01B21DD213814000;
}

// Глобальные переменные модуля
uint8_t gNode[6];		// Узел
uint16_t gClockSeq;		// Поле Clock sequence
uint64_t gLastTime = 0; // Время генерации последнего УУИд
int gLastUSNS = 0;		// Значение микро и наносекунд при генерации последнего УУИд

// Инициализация
bool generator_init(void) {
	uint32_t rnd;

	// Получаем значение поля Node - как случайное число
	if (!GetRND(&rnd)) {
		return false;
	}
	memmove(&gNode[0], &rnd, 4);
	if (!GetRND(&rnd)) {
		return false;
	}
	memmove(&gNode[4], &rnd, 2);
	gNode[5] |= 0x01;

	// Получаем значение поля Clock sequence
	if (!GetRND(&rnd)) {
		return false;
	}
	gClockSeq = rnd & 0x1FFF;

	return true;
}

// Генерация УУИд
// uuid - указатель на структуру uuid_t для помещения в нее данные УУИд
// Возвращаемый результат:
// 	- true - если УУИд создан успешно и помещен в структуру
// 	- false - если УУИд создать не удалось
bool uuid_create(uuid_t *uuid)
{
	uint64_t time;

	// Получение времени
	//
	// В течении 1 миллисекунды может быть сгенерировано
	// максимум 10000 уникальных УУИд.
	// Если в текущую миллисекунду уже сгенерировано 10000 УУИд,
	// будем ждать следующую миллисекунду.
	do {
		time = GetTime();
		if (!time) {
			return false;
		}
	} while (time == gLastTime && gLastUSNS == 9999);

	if (time == gLastTime) {
		// Если время не изменилось, за микро- и наносекунды
		// возьмем предыдущее значение плюс один
		gLastUSNS++;
	} else {
		// Если время изменилось, за микро- и наносекунды возьмём 0.
		gLastUSNS = 0;
		// и запомним значение времени
		gLastTime = time;
	}

	// Прибавим к значению времени (полученного с точностью до миллисекунд)
	// выбранное выше значение микро- и наносекунд
	time += gLastUSNS;

	// Заполним поля УУИд
	// Младшая часть времени
	uuid->time_low = (uint32_t)(time & 0xFFFFFFFF);
	// Средняя часть времени
	uuid->time_mid = (uint16_t)((time >> 32) & 0xFFFF);
	// Старшая часть времени
	uuid->time_hi_and_version = (uint16_t)((time >> 48) & 0x0FFF);
	// Версия
	uuid->time_hi_and_version |= (1 << 12);

	// Младшая часть временной последовательности
	uuid->clock_seq_low = (uint8_t)(gClockSeq & 0xFF);
	// Старшая часть временной последовательности
	uuid->clock_seq_hi_and_reserved = (uint8_t)((gClockSeq & 0x3F00) >> 8);
	// Вариант
	uuid->clock_seq_hi_and_reserved |= 0x80;

	// Узел
	memmove(&uuid->node[0], &gNode[0], 6);

	return true;
}
