/* The original file is taken from https://www.cbr.ru/ckki/assignment_unique_id/ */

/* =================================================================== *
 * Вариант реализации функции генерации первой части уникального       *
 * идентификатора договора (сделки) - универсального уникального       *
 * идентификатора в соответствии с указанием Банка России              *
 * "О правилах присвоения уникального идентификатора договора (сделки),*
 * по обязательствам из которого (из которой) формируется кредитная    *
 * история"                                                            *
 * =================================================================== */

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
HCRYPTPROV hCryptProv = 0;

#elif linux
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
int hRandom = -1;

#else
#error Platform not supported
#endif

#include "uuid.h"

bool generator_init(void);
bool GetRND(uint32_t *);
uint64_t GetTime(void);

#ifdef _WIN32
/* =================================================================== *
 * Реализация платформо-зависимых функций для ОС Windows               *
 * =================================================================== */

//Инициализация
bool uuid_init(void)
{
	if (CryptAcquireContextW(&hCryptProv, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) == 0)
	{
		return false;
	}
	//Инициализация платформо-независимой части
	return generator_init();
}

//Деинициализация (освобождение ресурсов)
void uuid_deinit(void)
{
	CryptReleaseContext(hCryptProv, 0);
	hCryptProv = 0;
}

//Получение 32-битного случайного числа
bool GetRND(uint32_t *rnd)
{
	if (CryptGenRandom(hCryptProv, sizeof(uint32_t), (void *)rnd) == 0)
	{
		return false;
	}
	return true;
}

//Получение времени в 100-наносекундных интервалах от 15.10.1580
uint64_t GetTime(void)
{
	SYSTEMTIME systime;
	uint64_t time;

	//получение системного времени
	GetSystemTime(&systime);
	//преобразование системного времени в формат FILETIME
	//(количество 100-наносекундных интервалов с 01.01.1601)
	if (!SystemTimeToFileTime(&systime, (LPFILETIME)&time))
		return 0;

	//дополнение разницей между 15.10.1580 и 01.01.1601
	//и возврат полученного результата
	return (time + 5748192000000000);
}

#elif linux
/* =================================================================== *
 * Реализация платформо-зависимых функций для ОС Linux                 *
 * =================================================================== */

//Инициализация
bool uuid_init(void)
{
	hRandom = open("/dev/random", O_RDONLY);
	if (hRandom < 0)
		return false;

	//Инициализация платформо-независимой части
	return generator_init();
}

//Деинициализация (освобождение ресурсов)
void uuid_deinit(void)
{
	close(hRandom);
	hRandom = -1;
}

//Получение 32-битного случайного числа
bool GetRND(uint32_t *rnd)
{
	if (read(hRandom, rnd, sizeof(uint32_t)) < 0)
		return false;
	return true;
}

//Получение времени в 100-наносекундных интервалах от 15.10.1580
uint64_t GetTime(void)
{
	struct timeval tp;

	if (gettimeofday(&tp, (struct timezone *)0) != 0)
		return 0;

	//Приводим время к 100-наносекундным интервалам от 15.10.1580,
	//при этом информацию о микросекундах обнуляем для совместимости
	//с Windows-версией.
	return ((uint64_t)tp.tv_sec * 10000000) + ((uint64_t)(tp.tv_usec / 1000) * 10000) + 0x01B21DD213814000;
}

#endif
/* =====================================================================
 */

//глобальные переменные модуля
uint8_t gNode[6];		//узел
uint16_t gClockSeq;		//поле Clock sequence
uint64_t gLastTime = 0; //время генерации последнего УУИд
int gLastUSNS = 0;		//значение микро и наносекунд при генерации последнего УУИд

//Инициализация
bool generator_init(void)
{
	uint32_t rnd;

	//Получаем значение поля Node - как случайное число
	if (!GetRND(&rnd))
		return false;
	memmove(&gNode[0], &rnd, 4);
	if (!GetRND(&rnd))
		return false;
	memmove(&gNode[4], &rnd, 2);
	gNode[5] |= 0x01;

	//Получаем значение поля Clock sequence
	if (!GetRND(&rnd))
		return false;
	gClockSeq = rnd & 0x1FFF;

	return true;
}

//Генерация УУИд
// uuid - указатель на структуру uuid_t для помещения в нее данные УУИд
//Возвращаемый результат:
//  true - если УУИд создан успешно и помещен в структуру
//  false - если УУИд создать не удалось
bool uuid_create(uuid_t *uuid)
{
	uint64_t time;

	//Получение времени
	//
	//В течении 1 миллисекунды может быть сгенерировано
	//максимум 10000 уникальных УУИд.
	//Если в текущую миллисекунду уже сгенерировано 10000 УУИд,
	//будем ждать следующую миллисекунду.
	do
	{
		time = GetTime();
		if (!time)
			return false;
	} while (time == gLastTime && gLastUSNS == 9999);

	if (time == gLastTime)
	{
		//Если время не изменилось, за микро- и наносекунды
		//возьмем предыдущее значение плюс один
		gLastUSNS++;
	}
	else
	{
		//Если время изменилось, за микро- и наносекунды возьмём 0.
		gLastUSNS = 0;
		//и запомним значение времени
		gLastTime = time;
	}

	//Прибавим к значению времени (полученного с точностью до миллисекунд)
	//выбранное выше значение микро- и наносекунд
	time += gLastUSNS;

	//заполним поля УУИд
	//младшая часть времени
	uuid->time_low = (uint32_t)(time & 0xFFFFFFFF);
	//средняя часть времени
	uuid->time_mid = (uint16_t)((time >> 32) & 0xFFFF);
	//старшая часть времени
	uuid->time_hi_and_version = (uint16_t)((time >> 48) & 0x0FFF);
	//версия
	uuid->time_hi_and_version |= (1 << 12);

	//младшая часть временной последовательности
	uuid->clock_seq_low = (uint8_t)(gClockSeq & 0xFF);
	//старшая часть временной последовательности
	uuid->clock_seq_hi_and_reserved = (uint8_t)((gClockSeq & 0x3F00) >> 8);
	//вариант
	uuid->clock_seq_hi_and_reserved |= 0x80;

	//узел
	memmove(&uuid->node[0], &gNode[0], 6);

	return true;
}
